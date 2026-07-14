# WellHouse ESP32 — Wire Protocols

The ESP32 is a **bridge with no sensors of its own**. It sits between:

```
                 WSS / JSON                         SPI (32-byte frames)
  [ Gateway ] <===============> [ ESP32 firmware ] <====================> [ STM Nucleo ]
      |                                                                    sensors, servo,
      v                                                                    breaker
  Firebase RTDB / Firestore
  Cloud Functions, FCM
```

There are two contracts here:

1. **WS envelope** — JSON between the firmware and your gateway. Your gateway maps
   these to the Firebase RTDB/Firestore paths in the API spec.
2. **SPI frame** — fixed 32-byte frames between the firmware (master) and the STM
   (slave). Your STM firmware implements the slave side.

All multi-byte integers are **little-endian** (both the ESP32 and STM32 are LE),
so packed structs are exchanged without byte swapping.

---

## 1. WS envelope (firmware ⇄ gateway)

Every message is a JSON text frame with a common envelope:

```json
{ "v": 1, "t": "<type>", "deviceId": "wh-0001", "ts": 1720900000000, "d": { ... } }
```

| Field      | Meaning                                                        |
|------------|----------------------------------------------------------------|
| `v`        | Protocol version (currently `1`).                              |
| `t`        | Message type (see below).                                     |
| `deviceId` | This device's ID (`CONFIG_WELLHOUSE_DEVICE_ID`).              |
| `ts`       | Epoch **milliseconds** on the device clock (SNTP-synced).     |
| `d`        | Type-specific payload.                                         |

`ts` is the device's own timestamp. For **queued offline events it is the original
event time**, not the reconnect time — the gateway should trust `ts` for ordering
and history, not its own receive time.

### 1a. Uplink (firmware → gateway)

| `t`         | `d` payload                                   | Gateway writes to (RTDB)                        |
|-------------|-----------------------------------------------|-------------------------------------------------|
| `hello`     | `{ token, fw }`                               | authenticate socket (no data write)             |
| `heartbeat` | `{ rssi }`                                    | `devices/{deviceId}/heartbeat` = `{ deviceId, timestamp, rssi }` |
| `water`     | `{ level_cm }`                                | `devices/{deviceId}/water/current` = `{ level_cm, timestamp }`   |
| `power`     | `{ powerState:"on"\|"cutoff", source:"auto"\|"manual" }` | `devices/{deviceId}/power`            |
| `cmdAck`    | `{ cmdId, result:"ok"\|"fail", detail }`      | `devices/{deviceId}/commands/{cmdId}/ack`       |
| `error`     | `{ code, detail }`                            | `errors/{errorId}` = `{ deviceId, code, detail, ts }` |

- `hello` is always the **first** frame on a (re)connect. The gateway must
  validate `token` before honouring anything else.
- Use the envelope `ts` as `timestamp` for `heartbeat`/`water` (or server time — but
  see the offline note above).
- `heartbeat` is sent every `CONFIG_WELLHOUSE_HEARTBEAT_MS` (default 30 s) and is
  **never** buffered offline.

Example:

```json
{ "v":1, "t":"water", "deviceId":"wh-0001", "ts":1720900000000, "d":{ "level_cm":37 } }
{ "v":1, "t":"power", "deviceId":"wh-0001", "ts":1720900001000, "d":{ "powerState":"cutoff", "source":"auto" } }
{ "v":1, "t":"cmdAck","deviceId":"wh-0001", "ts":1720900002000, "d":{ "cmdId":"c_abc", "result":"ok", "detail":"d=0" } }
```

### 1b. Downlink (gateway → firmware)

| `t`       | `d` payload                                          | Source                                   |
|-----------|------------------------------------------------------|------------------------------------------|
| `welcome` | `{ serverTs? }` (optional)                           | sent after a valid `hello`               |
| `command` | `{ cmdId, target:"power"\|"window", action:"cutoff"\|"restore", ts }` | RTDB `devices/{deviceId}/commands/{cmdId}` |
| `ping`    | *(none)*                                             | optional app-level keepalive             |

**Important — the gateway must add an `action` field.** The API spec's
`원격 차단 요청` (remote cutoff) and `원위치 해제` (reset/restore) both write to
`commands/{cmdId}` but carry different intent. The firmware cannot tell them apart
from `target` alone, so the gateway sets:

- `action:"cutoff"`  for a 원격 차단 요청
- `action:"restore"` for a 원위치 해제

The firmware forwards the command to the STM and replies with a `cmdAck` carrying
the **same `cmdId`**, within `CONFIG_WELLHOUSE_CMD_TIMEOUT_MS` (default 10 s). If the
STM stays silent, the firmware sends `result:"fail", detail:"timeout"`.

Example:

```json
{ "v":1, "t":"command", "d":{ "cmdId":"c_abc", "target":"power", "action":"cutoff", "ts":1720900002000 } }
```

### 1c. Connection & TLS

- URI: `CONFIG_WELLHOUSE_GATEWAY_URI` (`ws://` or `wss://`).
- `wss://` validates the server cert against the ESP-IDF CA bundle. Use a cert
  chained to a public root, or add your CA (see README).
- The client auto-reconnects (5 s backoff) and sends a WS ping every 20 s.

---

## 2. SPI frame (ESP32 master ⇄ STM slave)

- ESP32 is **master**; STM is **slave**.
- **DataReady** GPIO: STM → ESP32, **active high**. STM asserts it while it has one
  or more frames queued to send. The ESP32 also polls every 200 ms as a safety net.
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

| type   | value | direction     | payload struct                                        |
|--------|-------|---------------|-------------------------------------------------------|
| NOP    | 0x00  | either         | *(ignored)*                                          |
| WATER  | 0x10  | STM → ESP32    | `uint16 level_cm`                                    |
| POWER  | 0x11  | STM → ESP32    | `uint8 state (0=on,1=cutoff)`, `uint8 source (0=auto,1=manual)` |
| CMD_RESULT | 0x12 | STM → ESP32 | `uint16 local_cmd_id`, `uint8 result (0=ok,1=fail)`, `uint8 detail` |
| ERROR  | 0x13  | STM → ESP32    | `uint16 code`, `char detail[24]`                     |
| COMMAND | 0x20 | ESP32 → STM    | `uint16 local_cmd_id`, `uint8 target (0=power,1=window)`, `uint8 action (0=cutoff,1=restore)` |
| TIME_SYNC | 0x21 | ESP32 → STM  | `int64 epoch_ms`                                     |

### 2c. Command / ACK correlation

The WS `cmdId` is a string; SPI frames carry a small `local_cmd_id` (uint16) instead.
The ESP32 keeps the mapping:

1. Gateway → `command{cmdId:"c_abc", action:"cutoff"}`.
2. ESP32 allocates `local_cmd_id`, stores `local_cmd_id ↔ "c_abc"`, sends
   `COMMAND{local_cmd_id, target, action}` to the STM.
3. STM actuates, then replies `CMD_RESULT{local_cmd_id, result, detail}`.
4. ESP32 looks up `"c_abc"`, sends WS `cmdAck{cmdId:"c_abc", result, detail:"d=<detail>"}`.

The STM must **echo `local_cmd_id` unchanged** in `CMD_RESULT`. If no result arrives
within the ACK timeout, the ESP32 fails the ACK itself.

### 2d. STM slave checklist

- On every SPI transaction, clock out one 32-byte frame (NOP if nothing to report).
- Assert DataReady while ≥1 frame is queued; deassert when the queue is empty.
- Validate `sof` and `crc16` on received frames; ignore invalid ones.
- Handle `COMMAND` and `TIME_SYNC`; reply to each `COMMAND` with a `CMD_RESULT`.
- Emit `WATER` per your upload-rate rule, `POWER` on breaker state change, `ERROR`
  on faults.

---

## 3. Offline behaviour

While the WS link is down, uplink events (`water`, `power`, `error`, `cmdAck`) are
stored in NVS with their original `ts`, up to `CONFIG_WELLHOUSE_EVENT_QUEUE_CAP`
entries (oldest dropped when full). On reconnect, after `hello`, the firmware
flushes them oldest-first. `heartbeat` is never queued.
