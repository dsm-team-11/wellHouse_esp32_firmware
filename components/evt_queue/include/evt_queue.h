/*
 * evt_queue.h - persistent offline event queue.
 *
 * When the WS link is down, uplink events (water/power/error/cmdAck) are stored
 * here as their already-built JSON strings, so the original timestamp is kept.
 * On reconnect app_core drains the queue oldest-first. Backed by NVS, so it
 * survives reboots. Heartbeats are intentionally NOT queued - a stale liveness
 * ping has no value.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open the NVS-backed queue with a fixed ring capacity. */
int evt_queue_init(size_t capacity);

/* Number of pending events. */
size_t evt_queue_count(void);

/* Append a JSON event. If full, the oldest is dropped. Returns ESP_OK on store. */
int evt_queue_push(const char *json);

/* Copy the oldest event into buf. Returns ESP_OK, or ESP_ERR_NOT_FOUND if empty. */
int evt_queue_peek_oldest(char *buf, size_t buf_len);

/* Remove the oldest event (call after it has been sent successfully). */
int evt_queue_pop_oldest(void);

#ifdef __cplusplus
}
#endif
