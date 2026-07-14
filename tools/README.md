# tools — host-side dev broker + firmware simulator

Test the WellHouse **backend ⇄ firmware MQTT contract without an ESP32**. Node scripts,
no hardware, no Docker required.

```bash
npm install
```

## dev-broker.js

Minimal anonymous MQTT broker (aedes) on `tcp://0.0.0.0:1883` — a stand-in for
Mosquitto when it isn't installed. Logs every publish so you can watch the wire.

```bash
npm run broker            # or: node dev-broker.js
PORT=1884 node dev-broker.js
```

## firmware-sim.js

Emulates the firmware's north (MQTT) side per [`../docs/PROTOCOL.md`](../docs/PROTOCOL.md) §1:
publishes `water` (rising) + `heartbeat`, subscribes to `commands` / `control/wakeup`,
and ACKs each command to `devices/{id}/commands/{cmdId}/ack`.

```bash
node firmware-sim.js                       # demo-device-01 @ mqtt://localhost:1883
node firmware-sim.js --id demo-device-01 --broker mqtt://localhost:1883 \
     --water 5 --rise 8 --interval 3000 --hb 10000
node firmware-sim.js --fail-acks           # ACK every command as fail (error path)
```

| Flag | Meaning | Default |
| --- | --- | --- |
| `--id` | device id (MQTT client id + topic segment) | `demo-device-01` |
| `--broker` | broker URI | `mqtt://localhost:1883` |
| `--water` | starting level (cm) | `5` |
| `--rise` | cm added each tick (may be negative) | `2` |
| `--interval` | water publish period (ms) | `3000` |
| `--hb` | heartbeat period (ms) | `10000` |
| `--fail-acks` | ACK commands as `fail` | off |

Full walkthrough (broker + backend + sim + REST round-trip) in
[`../docs/INTEGRATION.md`](../docs/INTEGRATION.md).

> These are dev/test utilities. They are not flashed to the device and are not part
> of the ESP-IDF build.
