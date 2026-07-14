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

#include "ws_client.h"
#include "spi_link.h"
#include "evt_queue.h"
#include "wifi_mgr.h"

static const char *TAG = "app_core";

#define EVT_GREET      (1 << 0)
#define EVT_HEARTBEAT  (1 << 1)
#define PENDING_SLOTS  16
#define FLUSH_BUF_SIZE 512
#define APP_TASK_STACK 6144
#define APP_TASK_PRIO  5

typedef struct {
    bool     used;
    uint16_t local_id;
    char     cmd_id[40];
    int64_t  created_ms;
} pending_t;

static app_core_cfg_t   s_cfg;
static char             s_device_id[40];
static char             s_token[128];
static char             s_fw[16];

static EventGroupHandle_t s_events;
static SemaphoreHandle_t  s_pending_lock;
static pending_t          s_pending[PENDING_SLOTS];
static uint16_t           s_next_id = 1;
static bool               s_time_pushed;

/* ------------------------------------------------------------------ */
/*  Uplink helper: send if connected, otherwise queue. Frees `json`.   */
/* ------------------------------------------------------------------ */
static void uplink_take(char *json)
{
    if (!json) {
        return;
    }
    if (ws_client_send(json) != ESP_OK) {
        evt_queue_push(json);
    }
    free(json);
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
            uplink_take(ws_build_cmd_ack(s_device_id, now, cmd_id, RESULT_FAIL, "timeout"));

            xSemaphoreTake(s_pending_lock, portMAX_DELAY);
        }
    }
    xSemaphoreGive(s_pending_lock);
}

/* ------------------------------------------------------------------ */
/*  Callbacks                                                          */
/* ------------------------------------------------------------------ */
void app_core_on_ws_state(bool connected)
{
    if (connected) {
        xEventGroupSetBits(s_events, EVT_GREET);
    }
}

void app_core_on_ws_command(const ws_command_t *cmd)
{
    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    uint16_t local_id = s_next_id++;
    if (s_next_id == 0) {
        s_next_id = 1;
    }
    pending_t *p = pending_alloc(local_id, cmd->cmd_id);
    xSemaphoreGive(s_pending_lock);

    if (!p) {
        ESP_LOGE(TAG, "pending table full, rejecting %s", cmd->cmd_id);
        uplink_take(ws_build_cmd_ack(s_device_id, proto_now_ms(), cmd->cmd_id,
                                     RESULT_FAIL, "busy"));
        return;
    }

    ESP_LOGI(TAG, "command %s -> STM (target=%d action=%d id=%u)",
             cmd->cmd_id, cmd->target, cmd->action, local_id);
    if (spi_link_send_command(local_id, cmd->target, cmd->action) != ESP_OK) {
        char cmd_id[40];
        xSemaphoreTake(s_pending_lock, portMAX_DELAY);
        pending_take(local_id, cmd_id, sizeof(cmd_id));
        xSemaphoreGive(s_pending_lock);
        uplink_take(ws_build_cmd_ack(s_device_id, proto_now_ms(), cmd->cmd_id,
                                     RESULT_FAIL, "spi-busy"));
    }
}

void app_core_on_spi_frame(const spi_frame_t *f)
{
    int64_t now = proto_now_ms();
    switch (f->type) {
    case SPI_MSG_WATER:
        uplink_take(ws_build_water(s_device_id, now, f->p.water.level_cm));
        break;

    case SPI_MSG_POWER:
        uplink_take(ws_build_power(s_device_id, now,
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
        uplink_take(ws_build_cmd_ack(s_device_id, now, cmd_id, r, detail));
        break;
    }

    case SPI_MSG_ERROR: {
        char detail[SPI_PAYLOAD_SIZE]; /* frame field may be unterminated */
        memcpy(detail, f->p.error.detail, sizeof(f->p.error.detail));
        detail[sizeof(f->p.error.detail)] = '\0';
        uplink_take(ws_build_error(s_device_id, now, f->p.error.code, detail));
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
    if (!ws_client_connected()) {
        return; /* heartbeats are liveness-only; never queued */
    }
    char *json = ws_build_heartbeat(s_device_id, proto_now_ms(), wifi_mgr_rssi());
    if (json) {
        ws_client_send(json);
        free(json);
    }
}

static void greet_and_flush(void)
{
    /* hello must be the first frame the gateway sees on this socket. */
    char *hello = ws_build_hello(s_device_id, s_token, s_fw);
    if (hello) {
        ws_client_send(hello);
        free(hello);
    }

    char buf[FLUSH_BUF_SIZE];
    size_t drained = 0;
    while (evt_queue_peek_oldest(buf, sizeof(buf)) == ESP_OK) {
        if (ws_client_send(buf) != ESP_OK) {
            break; /* link dropped mid-flush; try again next connect */
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
            s_events, EVT_GREET | EVT_HEARTBEAT,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));

        if (bits & EVT_GREET) {
            greet_and_flush();
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
    strlcpy(s_token, cfg->token, sizeof(s_token));
    strlcpy(s_fw, cfg->fw_version, sizeof(s_fw));

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
