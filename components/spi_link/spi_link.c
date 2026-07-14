#include "spi_link.h"
#include "proto.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "spi_link";

#define TASK_STACK 4096
#define TASK_PRIO  6

static spi_device_handle_t s_dev;
static spi_water_cb_t      s_cb;
static int                 s_poll_ms;

static SemaphoreHandle_t s_lock;
static uint8_t           s_downlink[STM_FRAME_LEN]; /* [state, hour, minute] */

/* Reused DMA-capable transfer buffers. */
static uint8_t *s_dma_tx;
static uint8_t *s_dma_rx;

void spi_link_set_downlink(uint8_t state, uint8_t hour, uint8_t minute)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_downlink[0] = state;
    s_downlink[1] = hour;
    s_downlink[2] = minute;
    xSemaphoreGive(s_lock);
}

static void spi_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(s_dma_tx, s_downlink, STM_FRAME_LEN);
        xSemaphoreGive(s_lock);

        memset(s_dma_rx, 0, STM_FRAME_LEN);
        spi_transaction_t t = {
            .length = STM_FRAME_LEN * 8,
            .tx_buffer = s_dma_tx,
            .rx_buffer = s_dma_rx,
        };
        esp_err_t err = spi_device_transmit(s_dev, &t);
        if (err == ESP_OK && s_dma_rx[0] == STM_SYNC) {
            uint16_t adc = (uint16_t)(s_dma_rx[1] << 8) | s_dma_rx[2];
            if (s_cb) {
                s_cb(adc);
            }
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "transfer failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(s_poll_ms));
    }
}

int spi_link_init(const spi_link_cfg_t *cfg, spi_water_cb_t on_water)
{
    s_cb = on_water;
    s_poll_ms = cfg->poll_ms > 0 ? cfg->poll_ms : 100;

    s_lock = xSemaphoreCreateMutex();
    s_dma_tx = heap_caps_malloc(STM_FRAME_LEN, MALLOC_CAP_DMA);
    s_dma_rx = heap_caps_malloc(STM_FRAME_LEN, MALLOC_CAP_DMA);
    if (!s_lock || !s_dma_tx || !s_dma_rx) {
        return ESP_ERR_NO_MEM;
    }
    s_downlink[0] = RISK_SAFE;

    spi_bus_config_t bus = {
        .miso_io_num = cfg->pin_miso,
        .mosi_io_num = cfg->pin_mosi,
        .sclk_io_num = cfg->pin_sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = STM_FRAME_LEN,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(cfg->host, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = cfg->clock_hz,
        .mode = 0,
        .spics_io_num = cfg->pin_cs,
        .queue_size = 2,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(cfg->host, &dev, &s_dev));

    if (xTaskCreate(spi_task, "spi_link", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "SPI master up (host=%d cs=%d clk=%dHz poll=%dms)",
             cfg->host, cfg->pin_cs, cfg->clock_hz, s_poll_ms);
    return ESP_OK;
}
