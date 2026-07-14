/*
 * proto.h - WellHouse shared protocol definitions.
 *
 * Two wire protocols live here:
 *   1) MQTT     : topic + flat JSON between this firmware and the Spring Boot
 *                 backend (Mosquitto broker). See docs/API.md in the backend.
 *   2) STM link : a fixed 3-byte SPI exchange with the STM Nucleo (which is the
 *                 SPI slave). This matches the STM firmware's main.c as-is:
 *                   ESP32 -> STM : [ state(0..3), hour, minute ]
 *                   STM -> ESP32 : [ 0xAA, water_adc_hi, water_adc_lo ]
 *                 There is no CRC or message typing on this link - the STM's
 *                 format has no room for it. See docs/PROTOCOL.md.
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

/* Risk state byte sent to the STM (matches its SystemState enum). */
typedef enum {
    RISK_SAFE    = 0,
    RISK_WARNING = 1,
    RISK_ALERT   = 2,
    RISK_DANGER  = 3,
} risk_state_t;

/* ------------------------------------------------------------------ */
/*  1) STM 3-byte SPI link                                             */
/* ------------------------------------------------------------------ */
#define STM_SYNC       0xAAu   /* first byte the STM sends              */
#define STM_FRAME_LEN  3       /* bytes exchanged each transaction       */

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
