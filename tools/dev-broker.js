/*
 * dev-broker.js — minimal MQTT broker for local development.
 *
 * Stand-in for Mosquitto when Docker/mosquitto isn't installed. Anonymous, no TLS.
 * Listens on tcp://0.0.0.0:1883 so both the backend (Paho) and the firmware
 * simulator (or a real ESP32 on the LAN) can connect.
 *
 *   node dev-broker.js            # port 1883
 *   PORT=1884 node dev-broker.js  # custom port
 *
 * Logs every publish so you can watch the wire traffic during integration tests.
 */
import { createServer } from 'node:net'
import Aedes from 'aedes'

const PORT = Number(process.env.PORT || 1883)
const aedes = new Aedes()
const server = createServer(aedes.handle)

server.listen(PORT, () => {
  console.log(`[broker] MQTT listening on tcp://0.0.0.0:${PORT} (anonymous dev broker)`)
})

aedes.on('client', (c) => console.log(`[broker] + client connected: ${c?.id}`))
aedes.on('clientDisconnect', (c) => console.log(`[broker] - client disconnected: ${c?.id}`))

aedes.on('publish', (packet, client) => {
  // Skip $SYS keepalive noise; only show real device/app traffic.
  if (!client || !packet.topic || packet.topic.startsWith('$SYS')) return
  const body = packet.payload?.toString('utf8') ?? ''
  console.log(`[broker] ${client.id} -> ${packet.topic}  ${body}`)
})

for (const sig of ['SIGINT', 'SIGTERM']) {
  process.on(sig, () => {
    console.log('\n[broker] shutting down')
    server.close(() => aedes.close(() => process.exit(0)))
  })
}
