/*
 * spi_link.h - SPI master link to the STM Nucleo.
 *
 * The ESP32 is the SPI master. The STM raises a DataReady line when it has a
 * frame to hand up; the ESP32 also polls periodically as a safety net, and can
 * push command frames down at any time. Every exchange is one fixed 32-byte
 * frame in each direction (see spi_frame_t in proto.h), CRC-checked.
 */
#pragma once

#include "proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called from the SPI service task for each valid, non-NOP frame from the STM. */
typedef void (*spi_frame_cb_t)(const spi_frame_t *f);

typedef struct {
    int host;           /* SPI host, e.g. SPI2_HOST                     */
    int pin_miso;
    int pin_mosi;
    int pin_sclk;
    int pin_cs;
    int pin_dataready;  /* STM -> ESP32, active high, rising-edge IRQ   */
    int clock_hz;
} spi_link_cfg_t;

/* Bring up the bus, DataReady IRQ, TX queue and service task. */
int spi_link_init(const spi_link_cfg_t *cfg, spi_frame_cb_t on_frame);

/* Queue a COMMAND frame for the STM. local_cmd_id maps back to the MQTT cmdId. */
int spi_link_send_command(uint16_t local_cmd_id, cmd_target_t target);

/* Push the current epoch time to the STM (optional; call after SNTP sync). */
int spi_link_send_time_sync(int64_t epoch_ms);

/* Forward a weather-driven wakeup hint to the STM (no ACK expected). */
int spi_link_send_wakeup(uint16_t rainfall_mm_h);

#ifdef __cplusplus
}
#endif
