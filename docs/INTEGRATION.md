# Backend ⇄ Firmware Integration

How to bring the ESP32 firmware and the WellHouse backend up together over MQTT,
and how to test the whole link **without hardware** using the host-side simulator
in [`tools/`](../tools).

The wire contract (topics + payloads) is in [`PROTOCOL.md`](PROTOCOL.md) §1 and
matches the backend's `docs/API.md` §5 exactly — this doc is about *wiring them up*.

```
[ Backend ]  <== MQTT (tcp://host:1883) ==>  [ Broker ]  <==>  [ ESP32 or firmware-sim ]
 Spring Boot                                  Mosquitto /        emulates the firmware's
 wellhouse.mqtt.enabled=true                  dev-broker.js      north (MQTT) side
```

## 1. Start an MQTT broker

**Option A — Docker (matches prod):** from the backend repo,
```bash
docker compose up -d mosquitto     # eclipse-mosquitto:2 on :1883, config in mosquitto/mosquitto.conf
```

**Option B — no Docker/Mosquitto installed:** use the bundled Node dev broker,
```bash
cd tools && npm install
npm run broker                     # anonymous MQTT on tcp://0.0.0.0:1883, logs all traffic
```

## 2. Start the backend with MQTT enabled

The `dev` profile ships with MQTT **off**. Turn it on and point it at the broker:

```bash
cd backend
mvn spring-boot:run \
  -Dspring-boot.run.arguments="--wellhouse.mqtt.enabled=true --wellhouse.mqtt.broker-url=tcp://localhost:1883"
```

or via env vars (`WELLHOUSE_MQTT_ENABLED=true`, `WELLHOUSE_MQTT_URL=tcp://<host>:1883`).
On connect you should see `MQTT 구독 완료 (4 topics)`. The dev seed creates
`demo-device-01` (pairingCode `123456`).

> If two MQTT-enabled backends point at the same broker they share the client id
> `wellhouse-server` and will disconnect each other in a loop — run only one.

## 3a. Test with the firmware simulator (no hardware)

The simulator speaks the exact firmware MQTT contract — publishes `water`/`heartbeat`,
subscribes to `commands`/`control/wakeup`, and ACKs each command.

```bash
cd tools
node firmware-sim.js --id demo-device-01 --broker mqtt://localhost:1883 \
     --water 5 --rise 8 --interval 3000 --hb 10000
#   --fail-acks   ACK every command as fail (exercise the error path)
```

Then drive the backend REST API (register → pair → read state → command). A verified
run looks like:

| Step | Result |
| --- | --- |
| `GET /api/devices/demo-device-01/state` | `level: DANGER`, `riseCmPerMin: ~160`, `contributors.water: DANGER` — sim water ingested + risk engine ran |
| `POST /api/devices/demo-device-01/commands {target:"POWER"}` | `status: PENDING` → **`ACK_OK`**, `ackResult:"ok"`, `ackDetail:"d=0"` — command dispatched over MQTT, sim ACKed, backend correlated by `cmdId` |

Both uplink (firmware→backend) and downlink (backend→firmware) directions are covered.

## 3b. Run on real hardware

1. `idf.py menuconfig` → **WellHouse Configuration**:
   - **MQTT broker URI**: `mqtt://<backend-PC-LAN-IP>:1883` (e.g. `mqtt://10.162.191.207:1883`).
     Use the LAN IP, not `localhost` — the ESP32 connects over Wi-Fi.
   - **Wi-Fi SSID / password**: your 2.4 GHz network.
   - **Device ID**: leave `demo-device-01` (matches the dev seed) or set your own and
     seed it in the backend.
2. `idf.py set-target esp32 && idf.py build flash monitor`.
3. Ensure the backend broker is reachable from the ESP32's subnet (same LAN;
   allow TCP 1883 through the PC firewall).

Device identity is by topic (`devices/{id}/...`); the anonymous dev broker needs no
credentials. For an authenticated broker, username = `deviceId`, password = the
`deviceToken` from `POST /api/pair`.
