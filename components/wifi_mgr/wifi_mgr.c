#include "wifi_mgr.h"

#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

static const char *TAG = "wifi_mgr";

static volatile bool s_connected = false;
static volatile bool s_time_synced = false;
static bool s_sntp_started = false;
static char s_sntp_server[64] = "pool.ntp.org";

/* Time is "synced" once the year is sane; SNTP defaults to 1970 before that. */
static bool clock_is_set(void)
{
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    return (tm_info.tm_year + 1900) >= 2020;
}

static void on_time_sync(struct timeval *tv)
{
    (void)tv;
    s_time_synced = true;
    ESP_LOGI(TAG, "SNTP time synchronised");
}

static void start_sntp(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_sntp_server);
    config.sync_cb = on_time_sync;
    config.start = true;
    esp_netif_sntp_init(&config);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        ESP_LOGW(TAG, "disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        if (clock_is_set()) {
            s_time_synced = true;
        }
        if (!s_sntp_started) {
            start_sntp();          /* keep the clock disciplined even if RTC was set */
            s_sntp_started = true;
        }
    }
}

int wifi_mgr_start(const char *ssid, const char *password, const char *sntp_server)
{
    if (sntp_server && sntp_server[0]) {
        strlcpy(s_sntp_server, sntp_server, sizeof(s_sntp_server));
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        return err;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    /* An empty password implies an open AP. */
    if (password == NULL || password[0] == '\0') {
        cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to SSID \"%s\"", ssid);
    return ESP_OK;
}

bool wifi_mgr_connected(void)
{
    return s_connected;
}

bool wifi_mgr_time_synced(void)
{
    if (!s_time_synced && clock_is_set()) {
        s_time_synced = true;
    }
    return s_time_synced;
}

int wifi_mgr_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}
