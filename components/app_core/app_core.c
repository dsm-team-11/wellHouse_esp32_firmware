#include "app_core.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_err.h"

#include "mqtt_link.h"
#include "spi_link.h"
#include "evt_queue.h"
#include "wifi_mgr.h"

static const char *TAG = "app_core";

#define EVT_FLUSH      (1 << 0)
#define EVT_HEARTBEAT  (1 << 1)
#define PENDING_SLOTS  16
#define QUEUE_LINE_SEP '\n'
#define FLUSH_BUF_SIZE 512
#define APP_TASK_STACK 6144
#define APP_TASK_PRIO  5
#define MQTT_QOS       1

typedef struct {
    bool     used;
    uint16_t local_id;
    char     cmd_id[40];
    int64_t  created_ms;
} pending_t;

static app_core_cfg_t   s_cfg;
static char             s_device_id[40];

static EventGroupHandle_t s_events;
static SemaphoreHandle_t  s_pending_lock;
static pending_t          s_pending[PENDING_SLOTS];
static uint16_t           s_next_id = 1;
static bool               s_time_pushed;

/* ------------------------------------------------------------------ */
/*  Uplink: publish, or queue "topic\npayload" for later. Frees json.  */
/* ------------------------------------------------------------------ */
static void publish_or_queue(const char *topic, char *payload)
{
    if (!payload) {
        return;
    }
    if (mqtt_link_publish(topic, payload, MQTT_QOS, false) != ESP_OK) {
        char line[FLUSH_BUF_SIZE];
        int n = snprintf(line, sizeof(line), "%s%c%s", topic, QUEUE_LINE_SEP, payload);
        if (n > 0 && n < (int)sizeof(line)) {
            evt_queue_push(line);
        } else {
            ESP_LOGW(TAG, "event too large to queue (%d bytes), dropped", n);
        }
    }
    free(payload);
}

static void publish_ack(const char *cmd_id, cmd_result_t result, const char *detail)
{
    char topic[MQTT_TOPIC_MAX];
    mqtt_topic_ack(topic, sizeof(topic), s_device_id, cmd_id);
    publish_or_queue(topic, mqtt_payload_ack(result, detail));
}

/* ------------------------------------------------------------------ */
/*  Pending command table                                              */
/* ------------------------------------------------------------------ */
static pending_t *pending_alloc(uint16_t local_id, const char *cmd_id)
{
    for (int i = 0; i < PENDING_SLOTS; i++) {
        if (!s_pending[i].used) {
            s_pending[i].used = true;
            s_pending[i].local_id = local_id;
            strlcpy(s_pending[i].cmd_id, cmd_id, sizeof(s_pending[i].cmd_id));
            s_pending[i].created_ms = proto_now_ms();
            return &s_pending[i];
        }
    }
    return NULL;
}

static bool pending_take(uint16_t local_id, char *cmd_id_out, size_t out_len)
{
    for (int i = 0; i < PENDING_SLOTS; i++) {
        if (s_pending[i].used && s_pending[i].local_id == local_id) {
            strlcpy(cmd_id_out, s_pending[i].cmd_id, out_len);
            s_pending[i].used = false;
            return true;
        }
    }
    return false;
}

/* Fail-ACK any command the STM hasn't answered within cmd_timeout_ms. */
static void pending_sweep(void)
{
    int64_t now = proto_now_ms();
    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    for (int i = 0; i < PENDING_SLOTS; i++) {
        if (s_pending[i].used &&
            (now - s_pending[i].created_ms) > s_cfg.cmd_timeout_ms) {
            char cmd_id[40];
            strlcpy(cmd_id, s_pending[i].cmd_id, sizeof(cmd_id));
            s_pending[i].used = false;
            xSemaphoreGive(s_pending_lock);

            ESP_LOGW(TAG, "command %s timed out", cmd_id);
            publish_ack(cmd_id, RESULT_FAIL, "timeout");

            xSemaphoreTake(s_pending_lock, portMAX_DELAY);
        }
    }
    xSemaphoreGive(s_pending_lock);
}

/* ------------------------------------------------------------------ */
/*  Callbacks                                                          */
/* ------------------------------------------------------------------ */
void app_core_on_mqtt_state(bool connected)
{
    if (connected) {
        xEventGroupSetBits(s_events, EVT_FLUSH);
    }
}

void app_core_on_command(const mqtt_command_t *cmd)
{
    if (cmd->target == TARGET_UNKNOWN) {
        ESP_LOGW(TAG, "command %s has unknown target, rejecting", cmd->cmd_id);
        publish_ack(cmd->cmd_id, RESULT_FAIL, "unknown target");
        return;
    }

    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    uint16_t local_id = s_next_id++;
    if (s_next_id == 0) {
        s_next_id = 1;
    }
    pending_t *p = pending_alloc(local_id, cmd->cmd_id);
    xSemaphoreGive(s_pending_lock);

    if (!p) {
        ESP_LOGE(TAG, "pending table full, rejecting %s", cmd->cmd_id);
        publish_ack(cmd->cmd_id, RESULT_FAIL, "busy");
        return;
    }

    ESP_LOGI(TAG, "command %s -> STM (target=%d id=%u)", cmd->cmd_id, cmd->target, local_id);
    if (spi_link_send_command(local_id, cmd->target) != ESP_OK) {
        char cmd_id[40];
        xSemaphoreTake(s_pending_lock, portMAX_DELAY);
        pending_take(local_id, cmd_id, sizeof(cmd_id));
        xSemaphoreGive(s_pending_lock);
        publish_ack(cmd->cmd_id, RESULT_FAIL, "spi-busy");
    }
}

void app_core_on_wakeup(const mqtt_wakeup_t *wk)
{
    ESP_LOGI(TAG, "wakeup: region=%s rainfall=%.1fmm/h", wk->region, wk->rainfall_mm_h);
    uint16_t rain = (wk->rainfall_mm_h > 0) ? (uint16_t)(wk->rainfall_mm_h + 0.5) : 0;
    spi_link_send_wakeup(rain);
}

void app_core_on_spi_frame(const spi_frame_t *f)
{
    int64_t now = proto_now_ms();
    char topic[MQTT_TOPIC_MAX];

    switch (f->type) {
    case SPI_MSG_WATER:
        mqtt_topic_water(topic, sizeof(topic), s_device_id);
        publish_or_queue(topic, mqtt_payload_water(now, f->p.water.level_mm / 10.0));
        break;

    case SPI_MSG_POWER:
        mqtt_topic_power(topic, sizeof(topic), s_device_id);
        publish_or_queue(topic, mqtt_payload_power(now,
                                                   (power_state_t)f->p.power.state,
                                                   (power_source_t)f->p.power.source));
        break;

    case SPI_MSG_CMD_RESULT: {
        char cmd_id[40] = {0};
        xSemaphoreTake(s_pending_lock, portMAX_DELAY);
        bool found = pending_take(f->p.cmd_result.local_cmd_id, cmd_id, sizeof(cmd_id));
        xSemaphoreGive(s_pending_lock);
        if (!found) {
            ESP_LOGW(TAG, "cmd result for unknown id %u", f->p.cmd_result.local_cmd_id);
            break;
        }
        char detail[24];
        snprintf(detail, sizeof(detail), "d=%u", (unsigned)f->p.cmd_result.detail);
        cmd_result_t r = (f->p.cmd_result.result == RESULT_OK) ? RESULT_OK : RESULT_FAIL;
        publish_ack(cmd_id, r, detail);
        break;
    }

    case SPI_MSG_ERROR: {
        /* The backend has no error topic; log locally. */
        char detail[SPI_PAYLOAD_SIZE];
        memcpy(detail, f->p.error.detail, sizeof(f->p.error.detail));
        detail[sizeof(f->p.error.detail)] = '\0';
        ESP_LOGE(TAG, "STM error code=%u detail=%s", (unsigned)f->p.error.code, detail);
        break;
    }

    default:
        ESP_LOGW(TAG, "unhandled SPI frame type 0x%02x", f->type);
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Timers + task                                                      */
/* ------------------------------------------------------------------ */
static void heartbeat_timer_cb(void *arg)
{
    (void)arg;
    xEventGroupSetBits(s_events, EVT_HEARTBEAT);
}

static void send_heartbeat(void)
{
    if (!mqtt_link_connected()) {
        return; /* heartbeats are liveness-only; never queued */
    }
    char topic[MQTT_TOPIC_MAX];
    mqtt_topic_heartbeat(topic, sizeof(topic), s_device_id);
    char *payload = mqtt_payload_heartbeat(proto_now_ms(), wifi_mgr_rssi());
    if (payload) {
        mqtt_link_publish(topic, payload, MQTT_QOS, false);
        free(payload);
    }
}

static void flush_queue(void)
{
    char buf[FLUSH_BUF_SIZE];
    size_t drained = 0;
    while (evt_queue_peek_oldest(buf, sizeof(buf)) == ESP_OK) {
        char *sep = strchr(buf, QUEUE_LINE_SEP);
        if (!sep) {
            evt_queue_pop_oldest(); /* corrupt entry, discard */
            continue;
        }
        *sep = '\0';
        const char *topic = buf;
        const char *payload = sep + 1;
        if (mqtt_link_publish(topic, payload, MQTT_QOS, false) != ESP_OK) {
            break; /* link dropped mid-flush; retry next connect */
        }
        evt_queue_pop_oldest();
        drained++;
    }
    if (drained) {
        ESP_LOGI(TAG, "flushed %u queued events", (unsigned)drained);
    }
}

static void app_task(void *arg)
{
    (void)arg;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(
            s_events, EVT_FLUSH | EVT_HEARTBEAT,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));

        if (bits & EVT_FLUSH) {
            flush_queue();
        }
        if (bits & EVT_HEARTBEAT) {
            send_heartbeat();
        }

        /* Runs on the ~1s idle tick too. */
        pending_sweep();

        if (!s_time_pushed && wifi_mgr_time_synced()) {
            spi_link_send_time_sync(proto_now_ms());
            s_time_pushed = true;
        }
    }
}

int app_core_init(const app_core_cfg_t *cfg)
{
    s_cfg = *cfg;
    strlcpy(s_device_id, cfg->device_id, sizeof(s_device_id));

    s_events = xEventGroupCreate();
    s_pending_lock = xSemaphoreCreateMutex();
    if (!s_events || !s_pending_lock) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t targs = {
        .callback = heartbeat_timer_cb,
        .name = "heartbeat",
    };
    esp_timer_handle_t htimer;
    ESP_ERROR_CHECK(esp_timer_create(&targs, &htimer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(htimer, (uint64_t)s_cfg.heartbeat_ms * 1000));

    if (xTaskCreate(app_task, "app_core", APP_TASK_STACK, NULL, APP_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "app_core up (device=%s, hb=%dms)", s_device_id, s_cfg.heartbeat_ms);
    return ESP_OK;
}
