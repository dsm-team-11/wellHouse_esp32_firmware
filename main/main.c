/*
 * WellHouse ESP32 firmware - entry point.
 *
 * Role: a bridge with no sensors of its own. It relays data between the
 * WellHouse backend over MQTT and the STM Nucleo over SPI. See docs/PROTOCOL.md
 * for both wire protocols and README.md for the overall architecture.
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "wifi_mgr.h"
#include "mqtt_link.h"
#include "spi_link.h"
#include "evt_queue.h"
#include "app_core.h"

static const char *TAG = "main";

/*
 * Hold the MQTT connect until Wi-Fi is up. We also wait (briefly) for the clock,
 * so the first published events carry real timestamps, and so an mqtts:// TLS
 * handshake can validate certs.
 */
static void net_start_task(void *arg)
{
    (void)arg;

    while (!wifi_mgr_connected()) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    int waited = 0;
    while (!wifi_mgr_time_synced() && waited < 15000) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
    if (!wifi_mgr_time_synced()) {
        ESP_LOGW(TAG, "starting MQTT without confirmed time sync");
    }

    ESP_LOGI(TAG, "network ready, starting MQTT client");
    ESP_ERROR_CHECK(mqtt_link_start());
    vTaskDelete(NULL);
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    ESP_LOGI(TAG, "WellHouse firmware %s starting (device %s)",
             APP_FW_VERSION, APP_DEVICE_ID);

    init_nvs();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Offline queue first, so early SPI frames can be buffered if MQTT is down. */
    ESP_ERROR_CHECK(evt_queue_init(APP_EVENT_QUEUE_CAP));

    /* SPI link to the STM. */
    spi_link_cfg_t spi_cfg = {
        .host = APP_SPI_HOST,
        .pin_miso = APP_SPI_MISO,
        .pin_mosi = APP_SPI_MOSI,
        .pin_sclk = APP_SPI_SCLK,
        .pin_cs = APP_SPI_CS,
        .pin_dataready = APP_SPI_DATAREADY,
        .clock_hz = APP_SPI_CLOCK_HZ,
    };
    ESP_ERROR_CHECK(spi_link_init(&spi_cfg, app_core_on_spi_frame));

    /* Orchestration (heartbeat, routing, ACK timeouts). */
    app_core_cfg_t core_cfg = {
        .device_id = APP_DEVICE_ID,
        .heartbeat_ms = APP_HEARTBEAT_MS,
        .cmd_timeout_ms = APP_CMD_TIMEOUT_MS,
    };
    ESP_ERROR_CHECK(app_core_init(&core_cfg));

    /* MQTT transport (started later, once the network is ready). */
    mqtt_link_cfg_t mqtt_cfg = {
        .broker_uri = APP_MQTT_BROKER_URI,
        .device_id = APP_DEVICE_ID,
        .username = APP_MQTT_USERNAME,
        .password = APP_MQTT_PASSWORD,
    };
    ESP_ERROR_CHECK(mqtt_link_init(&mqtt_cfg,
                                   app_core_on_mqtt_state,
                                   app_core_on_command,
                                   app_core_on_wakeup));

    /* Wi-Fi + SNTP. */
    ESP_ERROR_CHECK(wifi_mgr_start(APP_WIFI_SSID, APP_WIFI_PASSWORD, APP_SNTP_SERVER));

    xTaskCreate(net_start_task, "net_start", 3072, NULL, 5, NULL);
}
