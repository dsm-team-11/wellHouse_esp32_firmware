/*
 * app_core.h - orchestration layer (STM 3-byte link edition).
 *
 * Because the STM link only carries [state,hour,minute] down and
 * [0xAA,water] up, this layer:
 *   - reads the STM's water ADC, converts it to cm, and publishes it to MQTT,
 *   - judges the risk state locally (thresholds) - the STM expects the ESP32 to,
 *   - sends that state plus the wall clock (hour:minute) back to the STM,
 *   - on a backend command, escalates the state to DANGER for a hold window so
 *     the STM actuates, and publishes a best-effort "ok" ACK (there is no real
 *     ACK channel over 3 bytes),
 *   - synthesises a power "cutoff" event when it drives DANGER,
 *   - publishes a heartbeat and flushes the offline queue on reconnect.
 *
 * main.c registers these callbacks with mqtt_link and spi_link.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *device_id;
    int heartbeat_ms;       /* e.g. 30000                                    */
    int water_publish_ms;   /* min interval between water publishes           */
    int cmd_hold_ms;        /* hold DANGER this long after a backend command  */
    int utc_offset_min;     /* local time offset for the STM clock (KST=540)   */
    int adc_zero;           /* ADC reading at 0 cm                            */
    int adc_per_cm;         /* ADC counts per cm                             */
    int thr_warning;        /* ADC thresholds for the risk state             */
    int thr_alert;
    int thr_danger;
} app_core_cfg_t;

int app_core_init(const app_core_cfg_t *cfg);

/* Callback for mqtt_link. */
void app_core_on_mqtt_state(bool connected);
void app_core_on_command(const mqtt_command_t *cmd);
void app_core_on_wakeup(const mqtt_wakeup_t *wk);

/* Callback for spi_link: a fresh water ADC reading from the STM. */
void app_core_on_water(uint16_t water_adc);

#ifdef __cplusplus
}
#endif
