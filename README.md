| Supported Target | ESP32 (WROOM-32) |
| ---------------- | ---------------- |

# WellHouse ESP32 Firmware

ESP-IDF **5.3.1** firmware for the WellHouse flood-safety device. The ESP32 has
**no sensors of its own** — it is a bridge that relays data between the WellHouse
backend over **MQTT** and an **STM Nucleo** board over SPI.

```
                 MQTT (JSON)                        SPI (32-byte frames)
  [ Backend ] <===============> [ ESP32 firmware ] <====================> [ STM Nucleo ]
      |         Mosquitto                                                  sensors, servo,
      v         tcp://host:1883                                           breaker, gate
  Spring Boot + Postgres/H2
  STOMP/WebSocket to the app, FCM
```

- **North side (MQTT):** flat JSON on `devices/{id}/...` topics to the backend's
  Mosquitto broker. Matches the backend contract in its `docs/API.md` §5.
- **South side (SPI):** the ESP32 is **master**; the STM asserts a **DataReady**
  line when it has data. Fixed 32-byte, CRC-checked frames.

Backend: [dsm-team-11/wellHouse-backend](https://github.com/dsm-team-11/wellHouse-backend)
(Spring Boot 3 / Java 17). The full wire contract for both sides is in
[`docs/PROTOCOL.md`](docs/PROTOCOL.md). Implement your STM firmware against it.

## What the firmware does

| Backend contract | Firmware behaviour |
| --- | --- |
| `devices/{id}/heartbeat` | Publishes `{rssi, timestamp}` every 30 s. |
| `devices/{id}/water` | Forwards STM `WATER` frames as `{level_cm, timestamp}`. |
| `devices/{id}/power` | Forwards STM `POWER` frames as `{powerState, source, timestamp}`. |
| `devices/{id}/commands` (sub) | Receives command, forwards `target` to STM. |
| `devices/{id}/commands/{cmdId}/ack` | Maps STM `CMD_RESULT` back to the UUID; 10 s timeout → fail. |
| `devices/{id}/control/wakeup` (sub) | Logs and forwards a wakeup hint to the STM. |
| offline | Buffers uplink events in NVS, flushes on reconnect (timestamps kept). |

Risk-stage / golden-time / FCM / reports logic stays in the **backend** — the
firmware only ships raw data and executes commands.

## Project layout

```
main/                 app_main, config (app_config.h), Kconfig, deps
components/
  proto/              MQTT topics + payloads, SPI frame definitions, JSON, CRC16
  wifi_mgr/           Wi-Fi station + reconnect + SNTP + RSSI
  mqtt_link/          MQTT transport (esp-mqtt: subscribe, publish, reconnect)
  spi_link/           SPI master, DataReady IRQ, TX queue, service task
  evt_queue/          NVS-backed offline event ring buffer
  app_core/           orchestration: routing, heartbeat, ACK timeouts
docs/PROTOCOL.md      authoritative wire spec (backend + STM teams)
```

## Build & flash

```bash
idf.py set-target esp32
idf.py menuconfig      # -> "WellHouse Configuration"
idf.py -p PORT flash monitor
```

### Configure (menuconfig → WellHouse Configuration)

- **Identity & network:** device ID, MQTT broker URI, MQTT username/password, Wi-Fi
  SSID/password, SNTP server.
- **Timing & storage:** heartbeat interval, command ACK timeout, offline queue capacity.
- **SPI link:** MISO/MOSI/SCLK/CS/DataReady GPIOs and clock. Defaults (ESP32 classic):
  MISO=19, MOSI=23, SCLK=18, CS=5, DataReady=4, 1 MHz.

### Talking to the backend

The backend's `dev` profile boots with **MQTT disabled** (H2, no broker). To test
firmware↔backend over MQTT, run the backend with the broker enabled:

```bash
# in the backend repo
docker compose up -d                                   # postgres + mosquitto
mvn spring-boot:run -Dspring-boot.run.profiles=prod    # or set WELLHOUSE_MQTT_ENABLED=true
```

Then point `WELLHOUSE_MQTT_BROKER_URI` at that broker (e.g. `mqtt://<pc-ip>:1883`).
The backend's dev seed creates `demo-device-01` (pairingCode `123456`) — a handy
default `WELLHOUSE_DEVICE_ID`. The dev Mosquitto is anonymous, so leave MQTT
username/password empty.

### TLS notes for `mqtts://`

`mqtts://` validates the broker certificate against the ESP-IDF CA bundle
(`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`, enabled in `sdkconfig.defaults`). For a private
CA, add it to the bundle. The clock is SNTP-synced before the first connection so
certificate validity checks pass.

## Not yet included (intentional next steps)

- **Pairing / deviceToken provisioning** — for an authenticated broker, the token
  comes from the app's pairing flow (`POST /api/pair`) and would be injected into
  NVS and used as the MQTT password.
- **Wi-Fi provisioning** (SoftAP/BLE) — credentials are currently Kconfig values.
- **Deep sleep** on `control/wakeup` — currently the hint is just forwarded to the STM.
- **STM firmware** — implement the SPI slave side per `docs/PROTOCOL.md`.
