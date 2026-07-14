/*
 * spi_link.h - 3-byte SPI master link to the STM Nucleo.
 *
 * The ESP32 is the SPI master; the STM is the slave. Every poll the ESP32 drives
 * one fixed 3-byte transaction (matching the STM's main.c):
 *     ESP32 -> STM : [ state(0..3), hour, minute ]
 *     STM -> ESP32 : [ 0xAA, water_adc_hi, water_adc_lo ]
 * There is no CRC, message typing, or DataReady line on this link. The STM's
 * blocking-slave timing means some transactions may be missed; the poll simply
 * repeats. See docs/PROTOCOL.md.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called from the SPI task whenever a valid (0xAA-framed) reading arrives. */
typedef void (*spi_water_cb_t)(uint16_t water_adc);

typedef struct {
    int host;        /* SPI host, e.g. SPI2_HOST */
    int pin_miso;
    int pin_mosi;
    int pin_sclk;
    int pin_cs;
    int clock_hz;
    int poll_ms;     /* transaction cadence */
} spi_link_cfg_t;

/* Bring up the SPI master and start the poll task. */
int spi_link_init(const spi_link_cfg_t *cfg, spi_water_cb_t on_water);

/* Update the bytes sent to the STM on the next poll (risk state + wall clock). */
void spi_link_set_downlink(uint8_t state, uint8_t hour, uint8_t minute);

#ifdef __cplusplus
}
#endif
