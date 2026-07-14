/*
 * mqtt_link.h - MQTT transport to the WellHouse backend.
 *
 * Thin wrapper over esp-mqtt. It owns the broker connection (auto-reconnect),
 * subscribes to this device's command + wakeup topics on connect, and hands
 * parsed downlink messages to callbacks. Pure transport - app_core builds the
 * uplink topics/payloads and drives the offline queue.
 */
#pragma once

#include <stdbool.h>
#include "proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called on every connect/disconnect edge. Keep it light. */
typedef void (*mqtt_state_cb_t)(bool connected);

/* Called for each downlink command / wakeup. Keep it light. */
typedef void (*mqtt_command_cb_t)(const mqtt_command_t *cmd);
typedef void (*mqtt_wakeup_cb_t)(const mqtt_wakeup_t *wk);

typedef struct {
    const char *broker_uri;   /* mqtt://host:1883 or mqtts://host:8883      */
    const char *device_id;    /* also used as the MQTT client id            */
    const char *username;     /* optional; NULL/"" -> anonymous (dev broker) */
    const char *password;     /* optional (e.g. the paired deviceToken)      */
} mqtt_link_cfg_t;

int  mqtt_link_init(const mqtt_link_cfg_t *cfg, mqtt_state_cb_t on_state,
                    mqtt_command_cb_t on_cmd, mqtt_wakeup_cb_t on_wakeup);
int  mqtt_link_start(void);
bool mqtt_link_connected(void);

/* Publish a payload to a topic. Returns ESP_OK, or an error if not connected. */
int  mqtt_link_publish(const char *topic, const char *payload, int qos, bool retain);

#ifdef __cplusplus
}
#endif
