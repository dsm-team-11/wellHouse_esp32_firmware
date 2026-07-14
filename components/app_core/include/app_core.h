/*
 * app_core.h - orchestration layer.
 *
 * Wires the transports together. It:
 *   - publishes a heartbeat every heartbeat_ms,
 *   - forwards STM frames (water/power/cmd-result) up to the backend on the
 *     matching MQTT topic,
 *   - forwards backend commands down to the STM, mapping the string cmdId to a
 *     small SPI id and back for the ACK,
 *   - fails a command's ACK if the STM doesn't answer within cmd_timeout_ms,
 *   - forwards wakeup hints to the STM,
 *   - flushes the offline queue on every (re)connect.
 *
 * main.c registers these callbacks with mqtt_link and spi_link.
 */
#pragma once

#include <stdbool.h>
#include "proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *device_id;
    int heartbeat_ms;          /* e.g. 30000                                */
    int cmd_timeout_ms;        /* e.g. 10000 - fail ACK if STM stays silent */
} app_core_cfg_t;

int app_core_init(const app_core_cfg_t *cfg);

/* Callback for mqtt_link: connection edge. */
void app_core_on_mqtt_state(bool connected);

/* Callback for mqtt_link: a downlink command arrived. */
void app_core_on_command(const mqtt_command_t *cmd);

/* Callback for mqtt_link: a wakeup hint arrived. */
void app_core_on_wakeup(const mqtt_wakeup_t *wk);

/* Callback for spi_link: a valid frame arrived from the STM. */
void app_core_on_spi_frame(const spi_frame_t *f);

#ifdef __cplusplus
}
#endif
