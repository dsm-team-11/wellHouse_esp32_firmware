/*
 * app_core.h - orchestration layer.
 *
 * Wires the transports together. It:
 *   - emits a heartbeat every heartbeat_ms,
 *   - forwards STM frames (water/power/error/cmd-result) up to the gateway,
 *   - forwards gateway commands down to the STM, mapping the string cmdId to a
 *     small SPI id and back for the ACK,
 *   - fails a command's ACK if the STM doesn't answer within cmd_timeout_ms,
 *   - sends hello and flushes the offline queue on every (re)connect.
 *
 * main.c registers these three callbacks with ws_client and spi_link.
 */
#pragma once

#include <stdbool.h>
#include "proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *device_id;
    const char *token;         /* gateway auth token / device secret        */
    const char *fw_version;
    int heartbeat_ms;          /* e.g. 30000                                */
    int cmd_timeout_ms;        /* e.g. 10000 - fail ACK if STM stays silent */
} app_core_cfg_t;

int app_core_init(const app_core_cfg_t *cfg);

/* Callback for ws_client: connection edge. */
void app_core_on_ws_state(bool connected);

/* Callback for ws_client: a downlink command arrived. */
void app_core_on_ws_command(const ws_command_t *cmd);

/* Callback for spi_link: a valid frame arrived from the STM. */
void app_core_on_spi_frame(const spi_frame_t *f);

#ifdef __cplusplus
}
#endif
