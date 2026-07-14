#include "ws_client.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ws_client";

#define WS_SEND_TIMEOUT_MS 2000
#define WS_MAX_RX_BYTES    4096   /* drop payloads larger than this */

static esp_websocket_client_handle_t s_client;
static ws_state_cb_t   s_on_state;
static ws_command_cb_t s_on_cmd;
static bool            s_connected;

/* Reassembly buffer for fragmented text frames (only touched in the WS task). */
static char  *s_rx;
static size_t s_rx_cap;

static void set_connected(bool up)
{
    if (up == s_connected) {
        return;
    }
    s_connected = up;
    ESP_LOGI(TAG, "%s", up ? "connected" : "disconnected");
    if (s_on_state) {
        s_on_state(up);
    }
}

static void handle_payload(const char *data, size_t len)
{
    ws_command_t cmd;
    ws_msg_type_t type = ws_parse(data, len, &cmd);
    switch (type) {
    case WS_DN_COMMAND:
        if (s_on_cmd) {
            s_on_cmd(&cmd);
        }
        break;
    case WS_DN_WELCOME:
        ESP_LOGI(TAG, "welcome from gateway");
        break;
    case WS_DN_PING:
        break;
    default:
        ESP_LOGW(TAG, "unhandled downlink frame");
        break;
    }
}

static void on_data(esp_websocket_event_data_t *d)
{
    /* Only text frames carry protocol messages. 0x01 = text, 0x00 = continuation. */
    if (d->op_code != 0x01 && d->op_code != 0x00) {
        return;
    }
    if (d->payload_len <= 0 || d->payload_len > WS_MAX_RX_BYTES) {
        if (d->payload_len > WS_MAX_RX_BYTES) {
            ESP_LOGW(TAG, "dropping oversized frame (%d bytes)", d->payload_len);
        }
        return;
    }

    /* Fast path: whole payload delivered in one event. */
    if (d->payload_offset == 0 && d->data_len == d->payload_len) {
        handle_payload(d->data_ptr, d->data_len);
        return;
    }

    /* Slow path: reassemble fragments. */
    if (d->payload_offset == 0) {
        if (s_rx_cap < (size_t)d->payload_len + 1) {
            char *bigger = realloc(s_rx, d->payload_len + 1);
            if (!bigger) {
                ESP_LOGE(TAG, "OOM reassembling %d bytes", d->payload_len);
                return;
            }
            s_rx = bigger;
            s_rx_cap = d->payload_len + 1;
        }
    }
    if (!s_rx || (size_t)d->payload_offset + d->data_len > s_rx_cap) {
        return; /* out-of-order fragment without a buffer; give up on this frame */
    }
    memcpy(s_rx + d->payload_offset, d->data_ptr, d->data_len);
    if (d->payload_offset + d->data_len >= d->payload_len) {
        handle_payload(s_rx, d->payload_len);
    }
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        set_connected(true);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        set_connected(false);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "transport error");
        break;
    case WEBSOCKET_EVENT_DATA:
        on_data(d);
        break;
    default:
        break;
    }
}

int ws_client_init(const char *uri, ws_state_cb_t on_state, ws_command_cb_t on_cmd)
{
    s_on_state = on_state;
    s_on_cmd = on_cmd;

    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .buffer_size = 2048,
        .ping_interval_sec = 20,
        .disable_auto_reconnect = false,
    };
    if (strncmp(uri, "wss", 3) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "init failed");
        return ESP_FAIL;
    }
    return esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                         ws_event_handler, NULL);
}

int ws_client_start(void)
{
    if (!s_client) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_websocket_client_start(s_client);
}

bool ws_client_connected(void)
{
    return s_client && esp_websocket_client_is_connected(s_client);
}

int ws_client_send(const char *json)
{
    if (!ws_client_connected()) {
        return ESP_FAIL;
    }
    int len = (int)strlen(json);
    int sent = esp_websocket_client_send_text(s_client, json, len,
                                              pdMS_TO_TICKS(WS_SEND_TIMEOUT_MS));
    return (sent == len) ? ESP_OK : ESP_FAIL;
}
