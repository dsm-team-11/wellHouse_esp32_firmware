#include "app_core.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

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
#define QUEUE_LINE_SEP '\n'
#define FLUSH_BUF_SIZE 512
#define APP_TASK_STACK 6144
#define APP_TASK_PRIO  5
#define MQTT_QOS       1

static app_core_cfg_t    s_cfg;
static char              s_device_id[40];

static EventGroupHandle_t s_events;
static SemaphoreHandle_t  s_lock;

/* Shared state (guarded by s_lock). */
static uint16_t      s_adc;
static risk_state_t  s_threshold_state = RISK_SAFE;
static risk_state_t  s_last_state = RISK_SAFE;
static int64_t       s_escalate_until;
static int64_t       s_last_water_pub_ms;

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

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */
static risk_state_t state_from_adc(uint16_t adc)
{
    if (adc >= s_cfg.thr_danger)  return RISK_DANGER;
    if (adc >= s_cfg.thr_alert)   return RISK_ALERT;
    if (adc >= s_cfg.thr_warning) return RISK_WARNING;
    return RISK_SAFE;
}

static double adc_to_cm(uint16_t adc)
{
    double cm = (double)((int)adc - s_cfg.adc_zero) / (double)s_cfg.adc_per_cm;
    return cm < 0 ? 0.0 : cm;
}

static void local_hm(uint8_t *hour, uint8_t *minute)
{
    time_t t = time(NULL) + (time_t)s_cfg.utc_offset_min * 60;
    struct tm tm_info;
    gmtime_r(&t, &tm_info);
    *hour = (uint8_t)tm_info.tm_hour;
    *minute = (uint8_t)tm_info.tm_min;
}

/*
 * Recompute the effective state, push it (+ clock) to the STM, and publish any
 * derived MQTT events. Publishing is done outside the lock.
 */
static void refresh(bool have_new_water, uint16_t adc)
{
    int64_t now = proto_now_ms();
    uint8_t hour, minute;
    local_hm(&hour, &minute);

    bool pub_water = false, pub_power = false;
    double level_cm = 0;
    risk_state_t st;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (have_new_water) {
        s_adc = adc;
        s_threshold_state = state_from_adc(adc);
        if (now - s_last_water_pub_ms >= s_cfg.water_publish_ms) {
            s_last_water_pub_ms = now;
            pub_water = true;
            level_cm = adc_to_cm(adc);
        }
    }
    bool escalating = now < s_escalate_until;
    st = s_threshold_state;
    if (escalating && st < RISK_DANGER) {
        st = RISK_DANGER;
    }
    if (st == RISK_DANGER && s_last_state != RISK_DANGER) {
        pub_power = true; /* synthesised: driving DANGER cuts the breaker */
    }
    s_last_state = st;
    xSemaphoreGive(s_lock);

    spi_link_set_downlink((uint8_t)st, hour, minute);

    if (pub_water) {
        char topic[MQTT_TOPIC_MAX];
        mqtt_topic_water(topic, sizeof(topic), s_device_id);
        publish_or_queue(topic, mqtt_payload_water(now, level_cm));
    }
    if (pub_power) {
        char topic[MQTT_TOPIC_MAX];
        mqtt_topic_power(topic, sizeof(topic), s_device_id);
        publish_or_queue(topic, mqtt_payload_power(now, POWER_CUTOFF, SRC_AUTO));
    }
}

/* ------------------------------------------------------------------ */
/*  Callbacks                                                          */
/* ------------------------------------------------------------------ */
void app_core_on_water(uint16_t water_adc)
{
    refresh(true, water_adc);
}

void app_core_on_mqtt_state(bool connected)
{
    if (connected) {
        xEventGroupSetBits(s_events, EVT_FLUSH);
    }
}

void app_core_on_command(const mqtt_command_t *cmd)
{
    const char *detail = "forced";
    /* Actuation commands escalate the STM to DANGER (it runs Emergency_Action). */
    if (cmd->target == TARGET_POWER || cmd->target == TARGET_WINDOW ||
        cmd->target == TARGET_WATER_GATE) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_escalate_until = proto_now_ms() + s_cfg.cmd_hold_ms;
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "command %s (target=%d): forcing DANGER (no per-target/ACK over 3-byte link)",
                 cmd->cmd_id, cmd->target);
        refresh(false, 0);
    } else {
        detail = "noop"; /* WAKEUP / unknown - nothing to actuate here */
    }

    /* Best-effort ACK: the STM gives no result over this link. */
    char topic[MQTT_TOPIC_MAX];
    mqtt_topic_ack(topic, sizeof(topic), s_device_id, cmd->cmd_id);
    publish_or_queue(topic, mqtt_payload_ack(RESULT_OK, detail));
}

void app_core_on_wakeup(const mqtt_wakeup_t *wk)
{
    ESP_LOGI(TAG, "wakeup: region=%s rainfall=%.1fmm/h (no STM channel, logged only)",
             wk->region, wk->rainfall_mm_h);
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

        /* ~1s tick: advance the STM clock and expire command escalation. */
        refresh(false, 0);
    }
}

int app_core_init(const app_core_cfg_t *cfg)
{
    s_cfg = *cfg;
    strlcpy(s_device_id, cfg->device_id, sizeof(s_device_id));

    s_events = xEventGroupCreate();
    s_lock = xSemaphoreCreateMutex();
    if (!s_events || !s_lock) {
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

    ESP_LOGI(TAG, "app_core up (device=%s, hb=%dms, thresholds=%d/%d/%d)",
             s_device_id, s_cfg.heartbeat_ms,
             s_cfg.thr_warning, s_cfg.thr_alert, s_cfg.thr_danger);
    return ESP_OK;
}
