/*
 * wifi_mgr.h - Wi-Fi station manager.
 *
 * Connects to a single AP, auto-reconnects on drop, and starts SNTP once an IP
 * is acquired so the rest of the firmware can produce real epoch timestamps.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up Wi-Fi in station mode and begin connecting. Expects NVS, the default
 * event loop, and netif to be initialised by the caller (see main.c). Once an IP
 * is acquired, SNTP is started against `sntp_server`. Returns ESP_OK once the
 * station is started (not necessarily connected yet).
 */
int wifi_mgr_start(const char *ssid, const char *password, const char *sntp_server);

/* True while the station holds an IP address. */
bool wifi_mgr_connected(void);

/* True once SNTP has stepped the clock to a real (post-2020) time. */
bool wifi_mgr_time_synced(void);

/* Current AP RSSI in dBm, or 0 if not connected. */
int wifi_mgr_rssi(void);

#ifdef __cplusplus
}
#endif
