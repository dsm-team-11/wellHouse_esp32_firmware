#include "mqtt_link.h"

#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "mqtt_client.h"
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

static const char *TAG = "mqtt_link";

static esp_mqtt_client_handle_t s_client;
static mqtt_state_cb_t   s_on_state;
static mqtt_command_cb_t s_on_cmd;
static mqtt_wakeup_cb_t  s_on_wakeup;
static volatile bool     s_connected;

static char s_topic_commands[MQTT_TOPIC_MAX];
static char s_topic_wakeup[MQTT_TOPIC_MAX];

static bool topic_is(const esp_mqtt_event_handle_t e, const char *want)
{
    size_t wlen = strlen(want);
    return e->topic && (size_t)e->topic_len == wlen &&
           strncmp(e->topic, want, wlen) == 0;
}

static void on_data(esp_mqtt_event_handle_t e)
{
    /* Commands/wakeup are small and arrive in a single data event. */
    if (e->current_data_offset != 0 || e->data_len != e->total_data_len) {
        ESP_LOGW(TAG, "ignoring fragmented downlink payload");
        return;
    }

    if (topic_is(e, s_topic_commands)) {
        mqtt_command_t cmd;
        if (mqtt_parse_command(e->data, e->data_len, &cmd) && s_on_cmd) {
            s_on_cmd(&cmd);
        }
    } else if (topic_is(e, s_topic_wakeup)) {
        mqtt_wakeup_t wk;
        if (mqtt_parse_wakeup(e->data, e->data_len, &wk) && s_on_wakeup) {
            s_on_wakeup(&wk);
        }
    }
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        esp_mqtt_client_subscribe(s_client, s_topic_commands, 1);
        esp_mqtt_client_subscribe(s_client, s_topic_wakeup, 1);
        s_connected = true;
        ESP_LOGI(TAG, "connected; subscribed to %s", s_topic_commands);
        if (s_on_state) s_on_state(true);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected");
        if (s_on_state) s_on_state(false);
        break;
    case MQTT_EVENT_DATA:
        on_data(e);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "transport error");
        break;
    default:
        break;
    }
}

int mqtt_link_init(const mqtt_link_cfg_t *cfg, mqtt_state_cb_t on_state,
                   mqtt_command_cb_t on_cmd, mqtt_wakeup_cb_t on_wakeup)
{
    s_on_state = on_state;
    s_on_cmd = on_cmd;
    s_on_wakeup = on_wakeup;

    mqtt_topic_sub_commands(s_topic_commands, sizeof(s_topic_commands), cfg->device_id);
    mqtt_topic_sub_wakeup(s_topic_wakeup, sizeof(s_topic_wakeup), cfg->device_id);

    esp_mqtt_client_config_t mcfg = {
        .broker.address.uri = cfg->broker_uri,
        .credentials.client_id = cfg->device_id,
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 5000,
    };
    if (cfg->username && cfg->username[0]) {
        mcfg.credentials.username = cfg->username;
        mcfg.credentials.authentication.password = cfg->password;
    }
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (strncmp(cfg->broker_uri, "mqtts", 5) == 0) {
        mcfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
#endif

    s_client = esp_mqtt_client_init(&mcfg);
    if (!s_client) {
        ESP_LOGE(TAG, "init failed");
        return ESP_FAIL;
    }
    return esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                          mqtt_event_handler, NULL);
}

int mqtt_link_start(void)
{
    if (!s_client) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_mqtt_client_start(s_client);
}

bool mqtt_link_connected(void)
{
    return s_connected;
}

int mqtt_link_publish(const char *topic, const char *payload, int qos, bool retain)
{
    if (!s_connected) {
        return ESP_FAIL;
    }
    int id = esp_mqtt_client_publish(s_client, topic, payload, 0 /*strlen*/, qos, retain);
    return id >= 0 ? ESP_OK : ESP_FAIL;
}
