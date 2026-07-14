/*
 * ws_client.h - WebSocket transport to the WellHouse gateway.
 *
 * Thin wrapper over esp_websocket_client: it owns the connection and TLS, keeps
 * auto-reconnect on, reassembles text frames, and hands parsed downlink commands
 * to a callback. It is pure transport - it does not build uplink messages or
 * know about the offline queue; app_core drives all of that.
 */
#pragma once

#include <stdbool.h>
#include "proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called from the WS task on every connect/disconnect edge. Keep it light. */
typedef void (*ws_state_cb_t)(bool connected);

/* Called from the WS task for each downlink command. Keep it light. */
typedef void (*ws_command_cb_t)(const ws_command_t *cmd);

/* Configure the client. `uri` may be ws:// or wss:// (wss uses the cert bundle). */
int ws_client_init(const char *uri, ws_state_cb_t on_state, ws_command_cb_t on_cmd);

/* Open the connection and keep it up (auto-reconnect). */
int ws_client_start(void);

/* True while the socket is connected. */
bool ws_client_connected(void);

/* Send a JSON text frame. Returns ESP_OK, or an error if not connected. */
int ws_client_send(const char *json);

#ifdef __cplusplus
}
#endif
