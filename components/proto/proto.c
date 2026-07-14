#include "proto.h"

#include <string.h>
#include <stdio.h>
#include <sys/time.h>

/* ------------------------------------------------------------------ */
/*  CRC + SPI frame helpers                                            */
/* ------------------------------------------------------------------ */
uint16_t proto_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) {
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

void spi_frame_finalize(spi_frame_t *f)
{
    f->sof = SPI_FRAME_SOF;
    f->crc16 = proto_crc16((const uint8_t *)f, SPI_FRAME_SIZE - 2);
}

bool spi_frame_valid(const spi_frame_t *f)
{
    if (f->sof != SPI_FRAME_SOF) {
        return false;
    }
    return f->crc16 == proto_crc16((const uint8_t *)f, SPI_FRAME_SIZE - 2);
}

/* ------------------------------------------------------------------ */
/*  Time                                                               */
/* ------------------------------------------------------------------ */
int64_t proto_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ------------------------------------------------------------------ */
/*  MQTT topics                                                        */
/* ------------------------------------------------------------------ */
void mqtt_topic_water(char *out, size_t n, const char *id)     { snprintf(out, n, "devices/%s/water", id); }
void mqtt_topic_heartbeat(char *out, size_t n, const char *id) { snprintf(out, n, "devices/%s/heartbeat", id); }
void mqtt_topic_power(char *out, size_t n, const char *id)     { snprintf(out, n, "devices/%s/power", id); }
void mqtt_topic_ack(char *out, size_t n, const char *id, const char *cmd_id)
{
    snprintf(out, n, "devices/%s/commands/%s/ack", id, cmd_id);
}
void mqtt_topic_sub_commands(char *out, size_t n, const char *id) { snprintf(out, n, "devices/%s/commands", id); }
void mqtt_topic_sub_wakeup(char *out, size_t n, const char *id)   { snprintf(out, n, "devices/%s/control/wakeup", id); }

/* ------------------------------------------------------------------ */
/*  MQTT payload builders                                              */
/* ------------------------------------------------------------------ */
static const char *power_state_str(power_state_t s) { return s == POWER_CUTOFF ? "cutoff" : "on"; }
static const char *power_src_str(power_source_t s)   { return s == SRC_MANUAL ? "manual" : "auto"; }
static const char *result_str(cmd_result_t r)        { return r == RESULT_FAIL ? "fail" : "ok"; }

static char *print_and_free(cJSON *d)
{
    if (!d) {
        return NULL;
    }
    char *out = cJSON_PrintUnformatted(d);
    cJSON_Delete(d);
    return out;
}

char *mqtt_payload_water(int64_t ts_ms, double level_cm)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return NULL;
    cJSON_AddNumberToObject(d, "level_cm", level_cm);
    cJSON_AddNumberToObject(d, "timestamp", (double)ts_ms);
    return print_and_free(d);
}

char *mqtt_payload_heartbeat(int64_t ts_ms, int rssi)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return NULL;
    cJSON_AddNumberToObject(d, "rssi", rssi);
    cJSON_AddNumberToObject(d, "timestamp", (double)ts_ms);
    return print_and_free(d);
}

char *mqtt_payload_power(int64_t ts_ms, power_state_t st, power_source_t src)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return NULL;
    cJSON_AddStringToObject(d, "powerState", power_state_str(st));
    cJSON_AddStringToObject(d, "source", power_src_str(src));
    cJSON_AddNumberToObject(d, "timestamp", (double)ts_ms);
    return print_and_free(d);
}

char *mqtt_payload_ack(cmd_result_t result, const char *detail)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return NULL;
    cJSON_AddStringToObject(d, "result", result_str(result));
    cJSON_AddStringToObject(d, "detail", detail ? detail : "");
    return print_and_free(d);
}

/* ------------------------------------------------------------------ */
/*  MQTT parsers                                                       */
/* ------------------------------------------------------------------ */
static cmd_target_t target_from_str(const char *s)
{
    if (!s) return TARGET_UNKNOWN;
    if (strcmp(s, "POWER") == 0)      return TARGET_POWER;
    if (strcmp(s, "WINDOW") == 0)     return TARGET_WINDOW;
    if (strcmp(s, "WATER_GATE") == 0) return TARGET_WATER_GATE;
    if (strcmp(s, "WAKEUP") == 0)     return TARGET_WAKEUP;
    return TARGET_UNKNOWN;
}

bool mqtt_parse_command(const char *json, size_t len, mqtt_command_t *out)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->target = TARGET_UNKNOWN;

    const cJSON *cid = cJSON_GetObjectItemCaseSensitive(root, "cmdId");
    if (cJSON_IsString(cid) && cid->valuestring) {
        strlcpy(out->cmd_id, cid->valuestring, sizeof(out->cmd_id));
    }
    const cJSON *tg = cJSON_GetObjectItemCaseSensitive(root, "target");
    out->target = target_from_str(cJSON_IsString(tg) ? tg->valuestring : NULL);
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(root, "ts");
    if (cJSON_IsNumber(ts)) {
        out->ts = (int64_t)ts->valuedouble;
    }

    bool ok = out->cmd_id[0] != '\0';
    cJSON_Delete(root);
    return ok;
}

bool mqtt_parse_wakeup(const char *json, size_t len, mqtt_wakeup_t *out)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const cJSON *rain = cJSON_GetObjectItemCaseSensitive(root, "rainfall_mm_h");
    if (cJSON_IsNumber(rain)) {
        out->rainfall_mm_h = rain->valuedouble;
    }
    const cJSON *region = cJSON_GetObjectItemCaseSensitive(root, "region");
    if (cJSON_IsString(region) && region->valuestring) {
        strlcpy(out->region, region->valuestring, sizeof(out->region));
    }
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(root, "timestamp");
    if (cJSON_IsNumber(ts)) {
        out->ts = (int64_t)ts->valuedouble;
    }

    cJSON_Delete(root);
    return true;
}
