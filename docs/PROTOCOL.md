# WellHouse ESP32 — Wire Protocols

The ESP32 is a **bridge with no sensors of its own**. It sits between:

```
                 MQTT (JSON)                        SPI (32-byte frames)
  [ Backend ] <===============> [ ESP32 firmware ] <====================> [ STM Nucleo ]
      |         Mosquitto                                                  sensors, servo,
      v         tcp://host:1883                                           breaker, gate
  Spring Boot + Postgres/H2
  STOMP/WebSocket to the app, FCM
```

There are two contracts here:

1. **MQTT** — topic + flat JSON between the firmware and the WellHouse backend.
   This matches the backend's `docs/API.md` §5 exactly.
2. **SPI frame** — fixed 32-byte frames between the firmware (master) and the STM
   (slave). Your STM firmware implements the slave side.

Both SPI peers are **little-endian** (ESP32 + STM32), so packed structs are
exchanged without byte swapping.

---

## 1. MQTT (firmware ⇄ backend)

- Broker: Mosquitto, `mqtt://<host>:1883` (dev is anonymous; prod should use
  auth/TLS). `mqtts://<host>:8883` uses the ESP-IDF CA bundle.
- MQTT client id = `deviceId`. For an authenticated broker: username = `deviceId`,
  password = the `deviceToken` from pairing.
- All payloads are flat JSON. The **message type is the topic**, not a field.
  Timestamps are epoch **milliseconds**. QoS 1.

### 1a. Publish (firmware → backend)

| Topic | Payload |
|-------|---------|
| `devices/{id}/water` | `{ "level_cm": 5.2, "timestamp": 1699999999999 }` |
| `devices/{id}/heartbeat` | `{ "rssi": -60, "timestamp": ... }` (every 30 s) |
| `devices/{id}/power` | `{ "powerState": "on"\|"cutoff", "source": "auto"\|"manual", "timestamp": ... }` |
| `devices/{id}/commands/{cmdId}/ack` | `{ "result": "ok"\|"fail", "detail": "..." }` |

- `level_cm` is a **decimal** (the firmware derives it from the STM's millimetre
  reading: `level_cm = level_mm / 10`).
- `{cmdId}` in the ack topic is the backend's UUID, echoed from the command.
- Heartbeats are **never** buffered offline; a stale liveness ping has no value.

### 1b. Subscribe (backend → firmware)

| Topic | Payload |
|-------|---------|
| `devices/{id}/commands` | `{ cmdId, target, ts, issuedBy, reason }` |
| `devices/{id}/control/wakeup` | `{ command:"wakeup", rainfall_mm_h, region, timestamp }` |

- `target` ∈ `POWER` \| `WINDOW` \| `WATER_GATE` \| `WAKEUP` (the backend
  `CommandTarget` enum). There is **no** cutoff/restore field — the target alone
  defines the action, and the STM decides how to actuate it.
- The firmware must ACK each command within **10 s** (backend `ACK_TIMEOUT`), by
  publishing to `devices/{id}/commands/{cmdId}/ack`. If the STM stays silent, the
  firmware self-publishes `{ "result":"fail", "detail":"timeout" }`.
- `control/wakeup` is a weather-driven hint (power-save wake). The firmware logs
  it and forwards it to the STM; no ACK.

Example round-trip:

```
backend →  devices/demo-device-01/commands
           { "cmdId":"7f3e...","target":"POWER","ts":1699999999999,"issuedBy":"uid_42","reason":"EMERGENCY" }
firmware → devices/demo-device-01/commands/7f3e.../ack
           { "result":"ok","detail":"d=0" }
```

### 1c. REST fallback (not implemented in this firmware)

The backend also exposes `POST /api/firmware/{id}/water|heartbeat|commands/{cmdId}/ack`
with a `Bearer <deviceToken>`. This firmware uses MQTT only; the REST path is a
documented fallback if you need it later.

---

## 2. SPI frame (ESP32 master ⇄ STM slave)

- ESP32 is **master**; STM is **slave**.
- **DataReady** GPIO: STM → ESP32, **active high**. STM asserts it while it has one
  or more frames queued. The ESP32 also polls every 200 ms as a safety net.
- Each transaction exchanges exactly one 32-byte frame in each direction (full
  duplex). If either side has nothing to send, it sends a `NOP` frame.

### 2a. Frame layout (32 bytes)

```
offset size field
0      1    sof     = 0xA5
1      1    type    (see table)
2      1    seq     rolling sequence (debug / dup detection)
3      1    flags   reserved, 0
4      26   payload (type-specific, see below)
30     2    crc16   CRC16-CCITT (poly 0x1021, init 0xFFFF) over bytes [0..29]
```

The C definition is `spi_frame_t` in [`components/proto/include/proto.h`](../components/proto/include/proto.h).

### 2b. Message types

| type   | value | direction     | payload |
|--------|-------|---------------|---------|
| NOP    | 0x00  | either         | *(ignored)* |
| WATER  | 0x10  | STM → ESP32    | `uint16 level_mm` (millimetres) |
| POWER  | 0x11  | STM → ESP32    | `uint8 state (0=on,1=cutoff)`, `uint8 source (0=auto,1=manual)` |
| CMD_RESULT | 0x12 | STM → ESP32 | `uint16 local_cmd_id`, `uint8 result (0=ok,1=fail)`, `uint8 detail` |
| ERROR  | 0x13  | STM → ESP32    | `uint16 code`, `char detail[24]` |
| COMMAND | 0x20 | ESP32 → STM    | `uint16 local_cmd_id`, `uint8 target (0=POWER,1=WINDOW,2=WATER_GATE,3=WAKEUP)` |
| TIME_SYNC | 0x21 | ESP32 → STM  | `int64 epoch_ms` |
| WAKEUP | 0x22  | ESP32 → STM    | `uint16 rainfall_mm_h` (no ACK) |

- **WATER is in millimetres.** The firmware converts to `level_cm` (÷10) for MQTT,
  preserving one decimal. If you need finer resolution, widen this field and adjust
  the conversion in `app_core.c`.

### 2c. Command / ACK correlation

The MQTT `cmdId` is a UUID string; SPI frames carry a small `local_cmd_id` (uint16).
The ESP32 keeps the mapping:

1. Backend → `command{cmdId:"7f3e...", target:"POWER"}`.
2. ESP32 allocates `local_cmd_id`, stores `local_cmd_id ↔ "7f3e..."`, sends
   `COMMAND{local_cmd_id, target}` to the STM.
3. STM actuates, then replies `CMD_RESULT{local_cmd_id, result, detail}`.
4. ESP32 looks up `"7f3e..."`, publishes `ack{result, detail:"d=<detail>"}`.

The STM must **echo `local_cmd_id` unchanged** in `CMD_RESULT`. If no result arrives
within the ACK timeout, the ESP32 fails the ACK itself.

### 2d. STM slave checklist

- On every SPI transaction, clock out one 32-byte frame (NOP if nothing to report).
- Assert DataReady while ≥1 frame is queued; deassert when the queue is empty.
- Validate `sof` and `crc16` on received frames; ignore invalid ones.
- Handle `COMMAND`, `TIME_SYNC`, `WAKEUP`; reply to each `COMMAND` with a `CMD_RESULT`.
- Emit `WATER` per your upload-rate rule, `POWER` on breaker state change, `ERROR`
  on faults.

---

## 3. Offline behaviour

While the broker is unreachable, uplink events (`water`, `power`, command `ack`) are
stored in NVS as `topic\npayload` with their original `timestamp`, up to
`CONFIG_WELLHOUSE_EVENT_QUEUE_CAP` entries (oldest dropped when full). On reconnect
the firmware flushes them oldest-first. `heartbeat` is never queued. STM `ERROR`
frames are logged locally only — the backend has no error topic.
