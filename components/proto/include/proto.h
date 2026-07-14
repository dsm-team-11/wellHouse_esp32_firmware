/*
 * proto.h - WellHouse shared protocol definitions.
 *
 * Two wire protocols live here:
 *   1) WS envelope  : JSON messages between this firmware and the WS gateway.
 *   2) SPI frame    : fixed 32-byte frames between this firmware (master) and
 *                     the STM Nucleo (slave).
 *
 * Both peers are little-endian (ESP32 + STM32), so packed structs are exchanged
 * without byte swapping. See docs/PROTOCOL.md for the authoritative spec.
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
typedef enum { TARGET_POWER = 0, TARGET_WINDOW = 1 } cmd_target_t;
typedef enum { ACTION_CUTOFF = 0, ACTION_RESTORE = 1 } cmd_action_t;
typedef enum { RESULT_OK = 0, RESULT_FAIL = 1 } cmd_result_t;

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
    SPI_MSG_COMMAND    = 0x20, /* actuate power/window                    */
    SPI_MSG_TIME_SYNC  = 0x21, /* epoch time push (optional)              */
} spi_msg_type_t;

typedef struct __attribute__((packed)) {
    uint8_t sof;    /* always SPI_FRAME_SOF                                */
    uint8_t type;   /* spi_msg_type_t                                     */
    uint8_t seq;    /* rolling sequence, for debugging/dup detection      */
    uint8_t flags;  /* reserved (0)                                       */
    union {
        struct __attribute__((packed)) { uint16_t level_cm; } water;
        struct __attribute__((packed)) { uint8_t state; uint8_t source; } power;
        struct __attribute__((packed)) { uint16_t local_cmd_id; uint8_t target; uint8_t action; } command;
        struct __attribute__((packed)) { uint16_t local_cmd_id; uint8_t result; uint8_t detail; } cmd_result;
        struct __attribute__((packed)) { uint16_t code; char detail[SPI_PAYLOAD_SIZE - 2]; } error;
        struct __attribute__((packed)) { int64_t epoch_ms; } time_sync;
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
/*  2) WS envelope protocol  (firmware <-> gateway)                    */
/* ------------------------------------------------------------------ */
typedef enum {
    WS_MSG_UNKNOWN = 0,
    /* uplink: firmware -> gateway */
    WS_UP_HELLO,
    WS_UP_HEARTBEAT,
    WS_UP_WATER,
    WS_UP_POWER,
    WS_UP_CMD_ACK,
    WS_UP_ERROR,
    /* downlink: gateway -> firmware */
    WS_DN_WELCOME,
    WS_DN_COMMAND,
    WS_DN_PING,
} ws_msg_type_t;

/* Parsed downlink command (WS_DN_COMMAND). */
typedef struct {
    ws_msg_type_t type;
    char          cmd_id[40];
    cmd_target_t  target;
    cmd_action_t  action;
    int64_t       ts;
} ws_command_t;

/*
 * Builders return a heap JSON string (caller frees with free()) or NULL on OOM.
 * ts_ms is epoch milliseconds; pass proto_now_ms() for "now", or a stored value
 * when replaying a queued offline event so the original timestamp is preserved.
 */
char *ws_build_hello(const char *device_id, const char *token, const char *fw);
char *ws_build_heartbeat(const char *device_id, int64_t ts_ms, int rssi);
char *ws_build_water(const char *device_id, int64_t ts_ms, int level_cm);
char *ws_build_power(const char *device_id, int64_t ts_ms, power_state_t st, power_source_t src);
char *ws_build_cmd_ack(const char *device_id, int64_t ts_ms, const char *cmd_id,
                       cmd_result_t result, const char *detail);
char *ws_build_error(const char *device_id, int64_t ts_ms, int code, const char *detail);

/* Parse a downlink frame. Returns the message type; fills *out for commands. */
ws_msg_type_t ws_parse(const char *json, size_t len, ws_command_t *out);

/* Epoch milliseconds from the system clock (valid once SNTP has synced). */
int64_t proto_now_ms(void);

#ifdef __cplusplus
}
#endif
