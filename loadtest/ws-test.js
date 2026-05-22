#!/usr/bin/env node
/* Quick WebSocket connectivity test.
 * Usage: node ws-test.js <session_id> <admin_token>
 */
import WebSocket from 'ws';

const sessionId = process.argv[2];
const token = process.argv[3];
if (!sessionId || !token) {
  console.error('Usage: node ws-test.js <session_id> <admin_token>');
  process.exit(1);
}

const url = `wss://api.rofidoesthings.site/ws/attendance/${sessionId}?token=${encodeURIComponent(token)}`;
console.log(`Connecting to ${url}`);

const ws = new WebSocket(url);

ws.on('open', () => console.log('✓ WebSocket connected'));
ws.on('message', (data) => {
  try {
    const msg = JSON.parse(data.toString());
    console.log(`[${msg.event}]`, JSON.stringify(msg));
  } catch (e) {
    console.log('Non-JSON message:', data.toString());
  }
});
ws.on('close', (code, reason) => {
  console.log(`Closed: code=${code} reason=${reason || 'none'}`);
  process.exit(0);
});
ws.on('error', (err) => console.error('Error:', err.message));

setTimeout(() => {
  console.log('15s timeout, closing test');
  ws.close();
}, 15000);
