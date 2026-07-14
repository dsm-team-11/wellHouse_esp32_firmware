#include "proto.h"

#include <string.h>
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
/*  WS envelope builders                                               */
/* ------------------------------------------------------------------ */
static const char *power_state_str(power_state_t s) { return s == POWER_CUTOFF ? "cutoff" : "on"; }
static const char *power_src_str(power_source_t s)   { return s == SRC_MANUAL ? "manual" : "auto"; }
static const char *result_str(cmd_result_t r)        { return r == RESULT_FAIL ? "fail" : "ok"; }

/*
 * Build "{v,t,deviceId,ts,d:{...}}". Takes ownership of `data` (freed on error,
 * attached on success). Returns malloc'd string or NULL.
 */
static char *envelope(const char *type, const char *device_id, int64_t ts, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        if (data) cJSON_Delete(data);
        return NULL;
    }
    cJSON_AddNumberToObject(root, "v", PROTO_VERSION);
    cJSON_AddStringToObject(root, "t", type);
    cJSON_AddStringToObject(root, "deviceId", device_id);
    cJSON_AddNumberToObject(root, "ts", (double)ts);
    if (data) {
        cJSON_AddItemToObject(root, "d", data);
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *ws_build_hello(const char *device_id, const char *token, const char *fw)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return NULL;
    cJSON_AddStringToObject(d, "token", token ? token : "");
    cJSON_AddStringToObject(d, "fw", fw ? fw : "");
    return envelope("hello", device_id, proto_now_ms(), d);
}

char *ws_build_heartbeat(const char *device_id, int64_t ts_ms, int rssi)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return NULL;
    cJSON_AddNumberToObject(d, "rssi", rssi);
    return envelope("heartbeat", device_id, ts_ms, d);
}

char *ws_build_water(const char *device_id, int64_t ts_ms, int level_cm)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return NULL;
    cJSON_AddNumberToObject(d, "level_cm", level_cm);
    return envelope("water", device_id, ts_ms, d);
}

char *ws_build_power(const char *device_id, int64_t ts_ms, power_state_t st, power_source_t src)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return NULL;
    cJSON_AddStringToObject(d, "powerState", power_state_str(st));
    cJSON_AddStringToObject(d, "source", power_src_str(src));
    return envelope("power", device_id, ts_ms, d);
}

char *ws_build_cmd_ack(const char *device_id, int64_t ts_ms, const char *cmd_id,
                       cmd_result_t result, const char *detail)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return NULL;
    cJSON_AddStringToObject(d, "cmdId", cmd_id ? cmd_id : "");
    cJSON_AddStringToObject(d, "result", result_str(result));
    cJSON_AddStringToObject(d, "detail", detail ? detail : "");
    return envelope("cmdAck", device_id, ts_ms, d);
}

char *ws_build_error(const char *device_id, int64_t ts_ms, int code, const char *detail)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return NULL;
    cJSON_AddNumberToObject(d, "code", code);
    cJSON_AddStringToObject(d, "detail", detail ? detail : "");
    return envelope("error", device_id, ts_ms, d);
}

/* ------------------------------------------------------------------ */
/*  WS downlink parser                                                 */
/* ------------------------------------------------------------------ */
ws_msg_type_t ws_parse(const char *json, size_t len, ws_command_t *out)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        return WS_MSG_UNKNOWN;
    }

    ws_msg_type_t type = WS_MSG_UNKNOWN;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "t");
    if (cJSON_IsString(t) && t->valuestring) {
        if (strcmp(t->valuestring, "command") == 0)      type = WS_DN_COMMAND;
        else if (strcmp(t->valuestring, "welcome") == 0) type = WS_DN_WELCOME;
        else if (strcmp(t->valuestring, "ping") == 0)    type = WS_DN_PING;
    }

    if (type == WS_DN_COMMAND && out) {
        memset(out, 0, sizeof(*out));
        out->type = type;
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "d");
        if (cJSON_IsObject(d)) {
            const cJSON *cid = cJSON_GetObjectItemCaseSensitive(d, "cmdId");
            if (cJSON_IsString(cid) && cid->valuestring) {
                strlcpy(out->cmd_id, cid->valuestring, sizeof(out->cmd_id));
            }
            const cJSON *tg = cJSON_GetObjectItemCaseSensitive(d, "target");
            out->target = (cJSON_IsString(tg) && tg->valuestring &&
                           strcmp(tg->valuestring, "window") == 0)
                              ? TARGET_WINDOW : TARGET_POWER;
            const cJSON *ac = cJSON_GetObjectItemCaseSensitive(d, "action");
            out->action = (cJSON_IsString(ac) && ac->valuestring &&
                           strcmp(ac->valuestring, "restore") == 0)
                              ? ACTION_RESTORE : ACTION_CUTOFF;
            const cJSON *ts = cJSON_GetObjectItemCaseSensitive(d, "ts");
            if (cJSON_IsNumber(ts)) {
                out->ts = (int64_t)ts->valuedouble;
            }
        }
    }

    cJSON_Delete(root);
    return type;
}
