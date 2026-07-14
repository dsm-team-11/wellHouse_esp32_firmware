# WellHouse ESP32 — Wire Protocols

The ESP32 is a **bridge with no sensors of its own**. It sits between:

```
                 MQTT (JSON)                        SPI (3-byte exchange)
  [ Backend ] <===============> [ ESP32 firmware ] <====================> [ STM Nucleo ]
      |         Mosquitto                                                  ADC water sensor,
      v         tcp://host:1883                                           servos, 7-seg, LCD
  Spring Boot + Postgres/H2
  STOMP/WebSocket to the app, FCM
```

Two contracts:

1. **MQTT** — topic + flat JSON between the firmware and the backend (matches the
   backend `docs/API.md` §5).
2. **STM link** — a fixed **3-byte SPI exchange** that matches the STM firmware's
   `main.c` as it stands (ESP32 = master, STM = slave).

> **Why only 3 bytes?** The STM firmware ([dsm-team-11/wellHouse_firmware](https://github.com/dsm-team-11/wellHouse_firmware))
> was written around a 3-byte exchange, so the ESP32 was adapted to it. This is
> simpler but **cannot carry per-target commands, real ACKs, or STM error/power
> reports** — see §2c. A richer 32-byte framed protocol (SOF+CRC+typing) is the
> better long-term option if the STM side can be updated.

---

## 1. MQTT (firmware ⇄ backend)

- Broker: Mosquitto, `mqtt://<host>:1883` (dev anonymous). `mqtts://` uses the CA bundle.
- MQTT client id = `deviceId`. Payloads are flat JSON; timestamps are epoch **ms**; QoS 1.

### 1a. Publish (firmware → backend)

| Topic | Payload | Notes |
|-------|---------|-------|
| `devices/{id}/water` | `{ "level_cm": 5.2, "timestamp": ... }` | from STM ADC via calibration; throttled |
| `devices/{id}/heartbeat` | `{ "rssi": -60, "timestamp": ... }` | every 30 s |
| `devices/{id}/power` | `{ "powerState":"cutoff", "source":"auto", "timestamp": ... }` | **synthesised** on DANGER entry |
| `devices/{id}/commands/{cmdId}/ack` | `{ "result":"ok", "detail":"forced" }` | **best-effort** (no real device ACK) |

### 1b. Subscribe (backend → firmware)

| Topic | Payload |
|-------|---------|
| `devices/{id}/commands` | `{ cmdId, target, ts, issuedBy, reason }` |
| `devices/{id}/control/wakeup` | `{ command, rainfall_mm_h, region, timestamp }` |

- `target` ∈ `POWER`/`WINDOW`/`WATER_GATE`/`WAKEUP`. Over the 3-byte link the ESP32
  cannot address a single actuator, so **any of POWER/WINDOW/WATER_GATE forces the
  STM to DANGER** for `CMD_HOLD_MS` (the STM then runs its all-servo `Emergency_Action`).
  `WAKEUP` is a no-op here.
- The ACK is published immediately as `ok` (`detail:"forced"`/`"noop"`) because the STM
  returns no result. Treat backend ACKs as "delivered", not "confirmed executed".
- `control/wakeup` is logged only (no STM channel).

---

## 2. STM link (3-byte SPI, ESP32 master ⇄ STM slave)

- ESP32 is **master**; STM is **slave** (`SPI_MODE_SLAVE`, Mode 0, 8-bit, HW NSS).
- Wiring: ESP32 SCLK→STM PB13, ESP32 MOSI→STM PB15, ESP32 MISO←STM PB14, ESP32 CS→STM PB12 (NSS).
- The ESP32 clocks one 3-byte transaction every `SPI_POLL_MS` (default 100 ms). No CRC,
  no framing, no DataReady line.

### 2a. The exchange

```
ESP32 -> STM (MOSI):  [ state, hour, minute ]
STM -> ESP32 (MISO):  [ 0xAA,  water_hi, water_lo ]
```

| Field | Meaning |
|-------|---------|
| `state` | risk 0=SAFE 1=WARNING 2=ALERT 3=DANGER. STM shows it on the LCD and, at ≥ALERT, runs `Emergency_Action` (breaker/gas/window servos). |
| `hour`,`minute` | wall clock (local time) for the STM 7-segment display. |
| `0xAA` | sync byte; the ESP32 ignores the reading if byte 0 ≠ 0xAA. |
| `water_hi`,`water_lo` | 12-bit raw ADC (0..4095), big-endian. |

### 2b. ESP32-side processing

- **water**: `level_cm = (adc - ADC_ZERO) / ADC_PER_CM` (clamped ≥0), published (throttled).
- **state**: judged locally from raw ADC — `≥THR_DANGER→3`, `≥THR_ALERT→2`,
  `≥THR_WARNING→1`, else `0`. A backend command escalates it to `3` for `CMD_HOLD_MS`.
- **clock**: `hour:minute` from the SNTP-synced clock + `UTC_OFFSET_MIN`.

### 2c. What this link cannot do (by design)

- No per-target actuation (POWER vs WINDOW vs WATER_GATE) — only a single state byte.
- No real command ACK — the STM sends no result; ACKs are best-effort `ok`.
- No STM-originated power state or error reports — power is synthesised, errors are absent.

If these matter, move to a framed protocol: give the STM a DataReady output, a
DMA/interrupt-driven always-ready slave, and 32-byte typed+CRC frames.

### 2d. Timing caveat

The STM currently services SPI with a **blocking** `HAL_SPI_TransmitReceive(...,50ms)`
inside a ~200 ms loop, so it is only listening part of the time. Keep `SPI_POLL_MS`
short so some transactions land in that window. For reliability the STM should switch
to `..._DMA`/`..._IT` and re-arm in the complete callback.

---

## 3. Offline behaviour

While the broker is unreachable, uplink events (`water`, `power`, `ack`) are stored in
NVS as `topic\npayload` with their original `timestamp`, up to
`CONFIG_WELLHOUSE_EVENT_QUEUE_CAP` entries (oldest dropped when full), and flushed
oldest-first on reconnect. `heartbeat` is never queued.
