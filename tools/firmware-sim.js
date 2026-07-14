/*
 * firmware-sim.js — host-side simulator of the WellHouse ESP32 firmware.
 *
 * Speaks the exact backend <-> firmware MQTT contract from docs/PROTOCOL.md §1
 * (and the backend's docs/API.md §5), so you can exercise the whole server link
 * without an ESP32 + STM on the bench.
 *
 * It behaves like the firmware's north (MQTT) side:
 *   publishes  devices/{id}/water      { level_cm, timestamp }   (periodic, rising)
 *              devices/{id}/heartbeat   { rssi, timestamp }        (every hb ms)
 *   subscribes devices/{id}/commands           -> ACKs within the timeout
 *              devices/{id}/control/wakeup      -> logs (no ACK)
 *   publishes  devices/{id}/commands/{cmdId}/ack { result, detail }
 *
 * Usage:
 *   node firmware-sim.js
 *   node firmware-sim.js --id demo-device-01 --broker mqtt://localhost:1883
 *   node firmware-sim.js --water 5 --rise 3 --interval 3000 --hb 10000
 *   node firmware-sim.js --fail-acks       # ACK every command as fail (test error path)
 */
import mqtt from 'mqtt'

function arg(name, def) {
  const i = process.argv.indexOf(`--${name}`)
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : def
}
const has = (name) => process.argv.includes(`--${name}`)

const DEVICE_ID = arg('id', 'demo-device-01')
const BROKER = arg('broker', 'mqtt://localhost:1883')
const HB_MS = Number(arg('hb', 10000))
const WATER_MS = Number(arg('interval', 3000))
const RISE = Number(arg('rise', 2)) // cm added each tick (can be negative)
const FAIL_ACKS = has('fail-acks')
let level = Number(arg('water', 5)) // starting level (cm)

const base = `devices/${DEVICE_ID}`
const now = () => Date.now()

const client = mqtt.connect(BROKER, {
  clientId: DEVICE_ID, // firmware uses deviceId as the MQTT client id
  clean: true,
  reconnectPeriod: 2000,
})

client.on('connect', () => {
  console.log(`[sim] connected to ${BROKER} as "${DEVICE_ID}"`)
  client.subscribe([`${base}/commands`, `${base}/control/wakeup`], { qos: 1 }, (err) => {
    if (err) console.error('[sim] subscribe failed:', err.message)
    else console.log(`[sim] subscribed: ${base}/commands, ${base}/control/wakeup`)
  })
  publishHeartbeat()
  publishWater()
})

client.on('reconnect', () => console.log('[sim] reconnecting...'))
client.on('error', (e) => console.error('[sim] error:', e.message))

client.on('message', (topic, payload) => {
  const body = payload.toString('utf8')
  const parts = topic.split('/')
  if (topic === `${base}/commands`) {
    let cmd = {}
    try { cmd = JSON.parse(body) } catch { /* ignore */ }
    console.log(`[sim] <- COMMAND ${cmd.target} (cmdId=${cmd.cmdId}, reason=${cmd.reason || '-'})`)
    // The real firmware relays to the STM over SPI and waits for CMD_RESULT.
    // Here we simulate a quick actuation and ACK back to the backend.
    const result = FAIL_ACKS ? 'fail' : 'ok'
    const detail = FAIL_ACKS ? 'sim-forced-fail' : 'd=0'
    setTimeout(() => {
      const ackTopic = `${base}/commands/${cmd.cmdId}/ack`
      pub(ackTopic, { result, detail })
      console.log(`[sim] -> ACK ${result} (${detail}) for cmdId=${cmd.cmdId}`)
    }, 300)
  } else if (topic === `${base}/control/wakeup`) {
    console.log(`[sim] <- WAKEUP hint ${body} (logged; no ACK)`)
  } else {
    console.log(`[sim] <- ${parts.slice(2).join('/')}: ${body}`)
  }
})

function pub(topic, obj) {
  client.publish(topic, JSON.stringify(obj), { qos: 1 })
}

function publishHeartbeat() {
  const rssi = -50 - Math.floor((level % 20)) // vary a bit, deterministic (no RNG)
  pub(`${base}/heartbeat`, { rssi, timestamp: now() })
  console.log(`[sim] -> heartbeat rssi=${rssi}`)
  setTimeout(publishHeartbeat, HB_MS)
}

function publishWater() {
  const levelCm = Math.round(level * 10) / 10 // one decimal, like level_mm/10
  pub(`${base}/water`, { level_cm: levelCm, timestamp: now() })
  console.log(`[sim] -> water ${levelCm}cm`)
  level = Math.max(0, level + RISE)
  setTimeout(publishWater, WATER_MS)
}

for (const sig of ['SIGINT', 'SIGTERM']) {
  process.on(sig, () => {
    console.log('\n[sim] disconnecting')
    client.end(true, () => process.exit(0))
  })
}
