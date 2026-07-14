#include "spi_link.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "spi_link";

#define TX_QUEUE_DEPTH 8
#define POLL_PERIOD_MS 200   /* safety-net poll even if DataReady is missed */
#define TASK_STACK     4096
#define TASK_PRIO      6

static spi_device_handle_t s_dev;
static spi_frame_cb_t      s_cb;
static QueueHandle_t       s_txq;
static SemaphoreHandle_t   s_wake;   /* given by IRQ and by send_command */
static int                 s_pin_dr;
static uint8_t             s_seq;

/* Reused DMA-capable transfer buffers. */
static uint8_t *s_dma_tx;
static uint8_t *s_dma_rx;

static void IRAM_ATTR dataready_isr(void *arg)
{
    (void)arg;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_wake, &hp);
    if (hp) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t do_xfer(const spi_frame_t *tx, spi_frame_t *rx)
{
    memcpy(s_dma_tx, tx, SPI_FRAME_SIZE);
    memset(s_dma_rx, 0, SPI_FRAME_SIZE);
    spi_transaction_t t = {
        .length = SPI_FRAME_SIZE * 8,
        .tx_buffer = s_dma_tx,
        .rx_buffer = s_dma_rx,
    };
    esp_err_t err = spi_device_transmit(s_dev, &t);
    if (err == ESP_OK) {
        memcpy(rx, s_dma_rx, SPI_FRAME_SIZE);
    }
    return err;
}

static void make_nop(spi_frame_t *f)
{
    memset(f, 0, sizeof(*f));
    f->type = SPI_MSG_NOP;
    f->seq = s_seq++;
    spi_frame_finalize(f);
}

static void spi_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* Wake on IRQ / queued command, or fall through to poll periodically. */
        xSemaphoreTake(s_wake, pdMS_TO_TICKS(POLL_PERIOD_MS));

        bool more = true;
        while (more) {
            spi_frame_t tx;
            if (xQueueReceive(s_txq, &tx, 0) != pdTRUE) {
                /* Nothing to send. If the STM has no data either, we're done. */
                if (gpio_get_level(s_pin_dr) == 0) {
                    break;
                }
                make_nop(&tx);
            }

            spi_frame_t rx;
            esp_err_t err = do_xfer(&tx, &rx);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "transfer failed: %s", esp_err_to_name(err));
                break;
            }
            if (!spi_frame_valid(&rx)) {
                ESP_LOGW(TAG, "bad frame (sof=0x%02x type=0x%02x)", rx.sof, rx.type);
            } else if (rx.type != SPI_MSG_NOP && s_cb) {
                s_cb(&rx);
            }

            more = (gpio_get_level(s_pin_dr) != 0) ||
                   (uxQueueMessagesWaiting(s_txq) > 0);
        }
    }
}

int spi_link_init(const spi_link_cfg_t *cfg, spi_frame_cb_t on_frame)
{
    s_cb = on_frame;
    s_pin_dr = cfg->pin_dataready;

    s_dma_tx = heap_caps_malloc(SPI_FRAME_SIZE, MALLOC_CAP_DMA);
    s_dma_rx = heap_caps_malloc(SPI_FRAME_SIZE, MALLOC_CAP_DMA);
    if (!s_dma_tx || !s_dma_rx) {
        ESP_LOGE(TAG, "DMA buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    s_txq = xQueueCreate(TX_QUEUE_DEPTH, sizeof(spi_frame_t));
    s_wake = xSemaphoreCreateBinary();
    if (!s_txq || !s_wake) {
        return ESP_ERR_NO_MEM;
    }

    spi_bus_config_t bus = {
        .miso_io_num = cfg->pin_miso,
        .mosi_io_num = cfg->pin_mosi,
        .sclk_io_num = cfg->pin_sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_FRAME_SIZE,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(cfg->host, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = cfg->clock_hz,
        .mode = 0,
        .spics_io_num = cfg->pin_cs,
        .queue_size = 4,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(cfg->host, &dev, &s_dev));

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << cfg->pin_dataready,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(cfg->pin_dataready, dataready_isr, NULL));

    if (xTaskCreate(spi_task, "spi_link", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "SPI master up (host=%d cs=%d dr=%d clk=%dHz)",
             cfg->host, cfg->pin_cs, cfg->pin_dataready, cfg->clock_hz);
    return ESP_OK;
}

int spi_link_send_command(uint16_t local_cmd_id, cmd_target_t target, cmd_action_t action)
{
    spi_frame_t f;
    memset(&f, 0, sizeof(f));
    f.type = SPI_MSG_COMMAND;
    f.seq = s_seq++;
    f.p.command.local_cmd_id = local_cmd_id;
    f.p.command.target = (uint8_t)target;
    f.p.command.action = (uint8_t)action;
    spi_frame_finalize(&f);

    if (xQueueSend(s_txq, &f, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "TX queue full, dropping command %u", local_cmd_id);
        return ESP_FAIL;
    }
    xSemaphoreGive(s_wake);
    return ESP_OK;
}

int spi_link_send_time_sync(int64_t epoch_ms)
{
    spi_frame_t f;
    memset(&f, 0, sizeof(f));
    f.type = SPI_MSG_TIME_SYNC;
    f.seq = s_seq++;
    f.p.time_sync.epoch_ms = epoch_ms;
    spi_frame_finalize(&f);

    if (xQueueSend(s_txq, &f, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_FAIL;
    }
    xSemaphoreGive(s_wake);
    return ESP_OK;
}
