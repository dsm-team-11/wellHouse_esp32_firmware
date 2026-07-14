| Supported Target | ESP32 (WROOM-32) |
| ---------------- | ---------------- |

# WellHouse ESP32 Firmware

ESP-IDF **5.3.1** firmware for the WellHouse flood-safety device. The ESP32 has
**no sensors of its own** — it bridges the WellHouse backend (over **MQTT**) and an
**STM Nucleo** board (over a 3-byte **SPI** exchange).

```
                 MQTT (JSON)                        SPI (3-byte exchange)
  [ Backend ] <===============> [ ESP32 firmware ] <====================> [ STM Nucleo ]
      |         Mosquitto                                                  ADC water sensor,
      v         tcp://host:1883                                           servos, 7-seg, LCD
  Spring Boot + Postgres/H2
  STOMP/WebSocket to the app, FCM
```

- **North side (MQTT):** flat JSON on `devices/{id}/...` topics to the backend
  ([dsm-team-11/wellHouse-backend](https://github.com/dsm-team-11/wellHouse-backend)).
- **South side (SPI):** ESP32 is master. Each poll it sends `[state, hour, minute]`
  and reads `[0xAA, waterHi, waterLo]` — matching the STM firmware
  ([dsm-team-11/wellHouse_firmware](https://github.com/dsm-team-11/wellHouse_firmware)) as-is.

Full wire contract: [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

> ⚠️ The 3-byte STM link is deliberately minimal. It carries water level, a risk-state
> byte, and a clock — but **not** per-target commands, real ACKs, or STM error/power
> reports. See "Known limitations" below.

## What the firmware does

| Function | Behaviour |
| --- | --- |
| Water level | Reads the STM's ADC, converts to `level_cm`, publishes `devices/{id}/water` (throttled). |
| Risk state | Judged **on the ESP32** from ADC thresholds; sent to the STM (it drives LCD + servos). |
| Clock | Sends `hour:minute` (SNTP + UTC offset) for the STM 7-segment display. |
| Heartbeat | Publishes `{rssi, timestamp}` every 30 s. |
| Commands | Backend command → forces STM to DANGER for a hold window; publishes a best-effort `ok` ACK. |
| Power | Synthesises a `cutoff` event when it drives DANGER. |
| Offline | Buffers uplink events in NVS, flushes on reconnect (timestamps kept). |

## Project layout

```
main/                 app_main, config (app_config.h), Kconfig, deps
components/
  proto/              MQTT topics + payloads, STM link constants, JSON
  wifi_mgr/           Wi-Fi station + reconnect + SNTP + RSSI
  mqtt_link/          MQTT transport (esp-mqtt: subscribe, publish, reconnect)
  spi_link/           SPI master, 3-byte poll exchange with the STM
  evt_queue/          NVS-backed offline event ring buffer
  app_core/           orchestration: state judgement, clock, routing, heartbeat
docs/PROTOCOL.md      authoritative wire spec (backend + STM)
```

## Build & flash

```bash
idf.py set-target esp32
idf.py menuconfig      # -> "WellHouse Configuration"
idf.py -p PORT flash monitor
```

### Configure (menuconfig → WellHouse Configuration)

- **Identity & network:** device ID, MQTT broker URI, MQTT username/password, Wi-Fi, SNTP.
- **Timing & storage:** heartbeat, water-publish throttle, command DANGER hold, UTC offset, queue cap.
- **Water: ADC mapping & risk thresholds:** ADC→cm calibration and the SAFE/WARNING/ALERT/DANGER
  ADC thresholds (defaults mirror the STM's `main.c`: 500 / 1200 / 2500).
- **SPI link:** MISO/MOSI/SCLK/CS GPIOs, clock, poll interval. Defaults (ESP32 classic):
  MISO=19, MOSI=23, SCLK=18, CS=5, 1 MHz, 100 ms poll.

Wiring: ESP32 SCLK→STM PB13, MOSI→STM PB15, MISO←STM PB14, CS→STM PB12 (NSS).

### Talking to the backend

The backend `dev` profile has MQTT **off**. To test over MQTT:

```bash
# in the backend repo
docker compose up -d                                   # postgres + mosquitto
mvn spring-boot:run -Dspring-boot.run.profiles=prod    # or WELLHOUSE_MQTT_ENABLED=true
```

Point `WELLHOUSE_MQTT_BROKER_URI` at that broker; the dev seed device is
`demo-device-01` (anonymous broker, so leave MQTT user/pass empty).

## Known limitations (3-byte link)

- **No per-target commands / no confirmed ACK.** A backend command coarsely forces
  DANGER (STM actuates all servos) and the ACK is best-effort `ok`.
- **Power events are synthesised**, and STM error reporting is not available.
- **STM slave timing is fragile** (blocking `HAL_SPI_TransmitReceive` in a slow loop);
  it should move to DMA/interrupt. Keep the ESP32 poll interval short.

To lift these, adopt a framed protocol (SOF+CRC+typing, DataReady line, DMA slave) on
both sides. `git log` has the earlier 32-byte framed implementation for reference.

## Not yet included

- Pairing / deviceToken provisioning, Wi-Fi provisioning (SoftAP/BLE), deep sleep.
- ADC→cm calibration is a rough linear default — tune to your sensor.
