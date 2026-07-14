| Supported Target | ESP32 (WROOM-32) |
| ---------------- | ---------------- |

# WellHouse ESP32 Firmware

ESP-IDF **5.3.1** firmware for the WellHouse flood-safety device. The ESP32 has
**no sensors of its own** — it is a bridge that relays data between a WebSocket
gateway (which fronts Firebase RTDB/Firestore) and an **STM Nucleo** board over SPI.

```
                 WSS / JSON                         SPI (32-byte frames)
  [ Gateway ] <===============> [ ESP32 firmware ] <====================> [ STM Nucleo ]
      |                                                                    sensors, servo,
      v                                                                    breaker
  Firebase RTDB / Firestore
```

- **North side (WebSocket):** a simple JSON protocol to your gateway. Firebase
  credentials never touch the device — the gateway holds them and maps messages
  to the RTDB/Firestore paths in the API spec.
- **South side (SPI):** the ESP32 is **master**; the STM asserts a **DataReady**
  line when it has data. Fixed 32-byte, CRC-checked frames.

The full wire contract for both sides is in [`docs/PROTOCOL.md`](docs/PROTOCOL.md).
Implement your gateway and STM firmware against that document.

## What the firmware does

Mapped to the API spec:

| API spec item                     | Firmware behaviour                                             |
| --------------------------------- | ------------------------------------------------------------- |
| 하트비트 (heartbeat)              | Sends `heartbeat` every 30 s with RSSI.                        |
| 수위 스트림 (water stream)        | Forwards STM `WATER` frames as `water` messages.              |
| 두꺼비집 상태 (power state)       | Forwards STM `POWER` frames as `power` messages.              |
| 원격 차단 / 원위치 해제 (commands)| Receives `command`, forwards to STM, ACKs with `cmdAck`.     |
| 명령 실행 결과 (command result)   | Maps STM `CMD_RESULT` back to the string `cmdId`; 10 s timeout.|
| 기기 에러 리포트 (error report)   | Forwards STM `ERROR` frames as `error` messages.             |
| 로컬 이벤트 큐 (offline queue)    | Buffers uplink events in NVS, flushes on reconnect (ts kept). |

Risk-stage / golden-time / FCM / reports logic stays **server-side** (Cloud
Functions), exactly as in the spec — the firmware only ships raw data and executes
commands.

## Project layout

```
main/                 app_main, config (app_config.h), Kconfig, deps
components/
  proto/              WS envelope + SPI frame definitions, JSON, CRC16
  wifi_mgr/           Wi-Fi station + reconnect + SNTP + RSSI
  ws_client/          WebSocket transport (TLS, reconnect, reassembly)
  spi_link/           SPI master, DataReady IRQ, TX queue, service task
  evt_queue/          NVS-backed offline event ring buffer
  app_core/           orchestration: routing, heartbeat, ACK timeouts
docs/PROTOCOL.md      authoritative wire spec (gateway + STM teams)
```

## Build & flash

```bash
idf.py set-target esp32
idf.py menuconfig      # -> "WellHouse Configuration"
idf.py -p PORT flash monitor
```

### Configure (menuconfig → WellHouse Configuration)

- **Identity & network:** device ID, gateway URI, device token, Wi-Fi SSID/password, SNTP server.
- **Timing & storage:** heartbeat interval, command ACK timeout, offline queue capacity.
- **SPI link:** MISO/MOSI/SCLK/CS/DataReady GPIOs and clock. Defaults (ESP32 classic):
  MISO=19, MOSI=23, SCLK=18, CS=5, DataReady=4, 1 MHz.

> The device token is a development placeholder. In production it comes from the
> `기기 페어링` flow (app → Cloud Function issues a custom token). Provision it into
> NVS or fetch it at pairing time rather than compiling it in.

### TLS notes for `wss://`

`wss://` validates the gateway certificate against the ESP-IDF CA bundle
(`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`, enabled in `sdkconfig.defaults`). If your
gateway uses a private CA, add it to the bundle or pin the cert in
`components/ws_client/ws_client.c`. The clock is SNTP-synced before the first
connection so certificate validity checks pass.

## Not yet included (intentional next steps)

- **Pairing / custom-token provisioning** — token is currently a Kconfig value.
- **Wi-Fi provisioning** (SoftAP/BLE) — credentials are currently Kconfig values.
- **STM firmware** — implement the slave side per `docs/PROTOCOL.md`.
- **Gateway server** — implement the Firebase mapping per `docs/PROTOCOL.md`.
