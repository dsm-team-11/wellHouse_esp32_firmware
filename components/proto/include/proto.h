/*
 * proto.h - WellHouse shared protocol definitions.
 *
 * Two wire protocols live here:
 *   1) MQTT      : topic + flat JSON between this firmware and the Spring Boot
 *                  backend (Mosquitto broker). See docs/API.md in the backend.
 *   2) SPI frame : fixed 32-byte frames between this firmware (master) and the
 *                  STM Nucleo (slave).
 *
 * Both SPI peers are little-endian (ESP32 + STM32), so packed structs are
 * exchanged without byte swapping. See docs/PROTOCOL.md for the full spec.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_VERSION 1

/* ------------------------------------------------------------------ */
/*  Shared enums                                                       */
/* ------------------------------------------------------------------ */
typedef enum { POWER_ON = 0, POWER_CUTOFF = 1 } power_state_t;
typedef enum { SRC_AUTO = 0, SRC_MANUAL = 1 } power_source_t;
typedef enum { RESULT_OK = 0, RESULT_FAIL = 1 } cmd_result_t;

/* Command targets, matching the backend CommandTarget enum names. */
typedef enum {
    TARGET_POWER      = 0,   /* "POWER"      - breaker cutoff  */
    TARGET_WINDOW     = 1,   /* "WINDOW"     - window servo    */
    TARGET_WATER_GATE = 2,   /* "WATER_GATE" - water barrier   */
    TARGET_WAKEUP     = 3,   /* "WAKEUP"     - leave low power  */
    TARGET_UNKNOWN    = 0xFF,
} cmd_target_t;

/* ------------------------------------------------------------------ */
/*  1) SPI link protocol  (ESP32 master <-> STM slave)                 */
/* ------------------------------------------------------------------ */
#define SPI_FRAME_SOF     0xA5u
#define SPI_FRAME_SIZE    32
#define SPI_PAYLOAD_SIZE  (SPI_FRAME_SIZE - 4 /*header*/ - 2 /*crc*/) /* 26 */

typedef enum {
    SPI_MSG_NOP        = 0x00, /* idle filler in either direction         */
    /* STM -> ESP32 */
    SPI_MSG_WATER      = 0x10, /* water level sample                      */
    SPI_MSG_POWER      = 0x11, /* breaker/power state change              */
    SPI_MSG_CMD_RESULT = 0x12, /* result of a command ESP32 sent to STM   */
    SPI_MSG_ERROR      = 0x13, /* STM-side fault report                   */
    /* ESP32 -> STM */
    SPI_MSG_COMMAND    = 0x20, /* actuate target                         */
    SPI_MSG_TIME_SYNC  = 0x21, /* epoch time push (optional)              */
    SPI_MSG_WAKEUP     = 0x22, /* weather-driven wake hint (no ACK)       */
} spi_msg_type_t;

typedef struct __attribute__((packed)) {
    uint8_t sof;    /* always SPI_FRAME_SOF                                */
    uint8_t type;   /* spi_msg_type_t                                     */
    uint8_t seq;    /* rolling sequence, for debugging/dup detection      */
    uint8_t flags;  /* reserved (0)                                       */
    union {
        struct __attribute__((packed)) { uint16_t level_mm; } water;              /* millimetres */
        struct __attribute__((packed)) { uint8_t state; uint8_t source; } power;
        struct __attribute__((packed)) { uint16_t local_cmd_id; uint8_t target; } command;
        struct __attribute__((packed)) { uint16_t local_cmd_id; uint8_t result; uint8_t detail; } cmd_result;
        struct __attribute__((packed)) { uint16_t code; char detail[SPI_PAYLOAD_SIZE - 2]; } error;
        struct __attribute__((packed)) { int64_t epoch_ms; } time_sync;
        struct __attribute__((packed)) { uint16_t rainfall_mm_h; } wakeup;
        uint8_t raw[SPI_PAYLOAD_SIZE];
    } p;
    uint16_t crc16; /* CRC16-CCITT over bytes [0 .. SPI_FRAME_SIZE-3]     */
} spi_frame_t;

_Static_assert(sizeof(spi_frame_t) == SPI_FRAME_SIZE, "spi_frame_t must be 32 bytes");

/* CRC16-CCITT (poly 0x1021, init 0xFFFF). */
uint16_t proto_crc16(const uint8_t *data, size_t len);

/* Stamp SOF and CRC into a frame whose type/payload are already set. */
void spi_frame_finalize(spi_frame_t *f);

/* True when SOF and CRC both check out. */
bool spi_frame_valid(const spi_frame_t *f);

/* ------------------------------------------------------------------ */
/*  2) MQTT protocol  (firmware <-> backend)                           */
/* ------------------------------------------------------------------ */
/* Longest topic: devices/{id}/commands/{uuid}/ack - keep generous headroom. */
#define MQTT_TOPIC_MAX 160

/* Topic builders. `out` must hold MQTT_TOPIC_MAX bytes. */
void mqtt_topic_water(char *out, size_t n, const char *device_id);
void mqtt_topic_heartbeat(char *out, size_t n, const char *device_id);
void mqtt_topic_power(char *out, size_t n, const char *device_id);
void mqtt_topic_ack(char *out, size_t n, const char *device_id, const char *cmd_id);
void mqtt_topic_sub_commands(char *out, size_t n, const char *device_id);
void mqtt_topic_sub_wakeup(char *out, size_t n, const char *device_id);

/*
 * Flat-payload builders. Return a heap JSON string (free() it) or NULL on OOM.
 * ts_ms is epoch milliseconds; pass proto_now_ms() for "now" or a stored value
 * when replaying a queued offline event so the original timestamp is preserved.
 */
char *mqtt_payload_water(int64_t ts_ms, double level_cm);
char *mqtt_payload_heartbeat(int64_t ts_ms, int rssi);
char *mqtt_payload_power(int64_t ts_ms, power_state_t st, power_source_t src);
char *mqtt_payload_ack(cmd_result_t result, const char *detail);

/* Parsed downlink command from devices/{id}/commands. */
typedef struct {
    char         cmd_id[40];   /* backend UUID */
    cmd_target_t target;
    int64_t      ts;
} mqtt_command_t;

/* Parsed wakeup from devices/{id}/control/wakeup. */
typedef struct {
    double  rainfall_mm_h;
    char    region[32];
    int64_t ts;
} mqtt_wakeup_t;

/* Parse helpers. Return true on success. */
bool mqtt_parse_command(const char *json, size_t len, mqtt_command_t *out);
bool mqtt_parse_wakeup(const char *json, size_t len, mqtt_wakeup_t *out);

/* Epoch milliseconds from the system clock (valid once SNTP has synced). */
int64_t proto_now_ms(void);

#ifdef __cplusplus
}
#endif
