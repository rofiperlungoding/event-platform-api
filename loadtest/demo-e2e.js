#!/usr/bin/env node
/**
 * End-to-end demo: simulates a complete event lifecycle.
 *
 *   1. Admin logs in
 *   2. Admin creates an event ("Demo Event")
 *   3. Admin creates a scheduled session ("Opening Plenary")
 *   4. Admin opens a WebSocket to watch live check-ins
 *   5. 5 participants register (bulk import)
 *   6. Each participant logs in and checks in
 *   7. WebSocket prints live stream
 *   8. Admin downloads CSV export
 *   9. Print final stats
 */
import WebSocket from 'ws';

const API = 'https://api.rofidoesthings.site';
const ADMIN_EMAIL = 'admin@intrivia.test';
const ADMIN_PASSWORD = 'admin123';

const log = (...args) => console.log(`[${new Date().toLocaleTimeString()}]`, ...args);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function api(method, path, opts = {}) {
  const r = await fetch(`${API}${path}`, {
    method,
    headers: { 'Content-Type': 'application/json', ...(opts.headers || {}) },
    body: opts.body,
  });
  const text = await r.text();
  if (!r.ok) throw new Error(`${method} ${path} → ${r.status}: ${text}`);
  return text ? JSON.parse(text) : null;
}

async function main() {
  console.log('━'.repeat(70));
  console.log('Event Platform — End-to-End Demo');
  console.log('━'.repeat(70));

  // ── 1. Admin login ────────────────────────────────────────────────
  log('Step 1: admin login');
  const adminAuth = await api('POST', '/auth/login', {
    body: JSON.stringify({ email: ADMIN_EMAIL, password: ADMIN_PASSWORD }),
  });
  log(`  ✓ admin authenticated (id=${adminAuth.participant.id}, role=${adminAuth.participant.role})`);
  const adminToken = adminAuth.token;
  const adminHeaders = { Authorization: `Bearer ${adminToken}` };

  // ── 2. Create event ───────────────────────────────────────────────
  log('Step 2: create event');
  const slug = `demo-${Date.now()}`;
  const event = await api('POST', '/events', {
    headers: adminHeaders,
    body: JSON.stringify({
      slug,
      name: 'End-to-End Demo Event',
      description: 'Demonstrating full platform capabilities',
    }),
  });
  log(`  ✓ event created (id=${event.id}, slug=${event.slug})`);

  // ── 3. Create attendance session ──────────────────────────────────
  log('Step 3: create session');
  const session = await api('POST', '/sessions/create', { headers: adminHeaders });
  log(`  ✓ session ${session.id} (code=${session.code}, expires=${session.expires_at})`);

  // ── 4. Open WebSocket for live feed ───────────────────────────────
  log('Step 4: open WebSocket admin feed');
  const wsUrl = `${API.replace('https://', 'wss://')}/ws/attendance/${session.id}?token=${encodeURIComponent(adminToken)}`;
  const ws = new WebSocket(wsUrl);
  const wsEvents = [];
  ws.on('message', (data) => {
    try {
      const msg = JSON.parse(data.toString());
      wsEvents.push(msg);
      if (msg.event === 'checkin') {
        log(`  📡 [WS] ${msg.name} (${msg.team}) checked in via ${msg.device_id}`);
      } else if (msg.event === 'connected') {
        log(`  📡 [WS] connected (session_id=${msg.session_id})`);
      }
    } catch (_) {}
  });
  await new Promise((resolve) => ws.on('open', resolve));
  await sleep(500);

  // ── 5. Bulk-register 5 participants ───────────────────────────────
  log('Step 5: bulk-register 5 participants (CSV upload)');
  const stamp = Date.now();
  const participants = ['alice', 'bob', 'charlie', 'dani', 'evan'];
  const csv = [
    'name,email,team,password',
    ...participants.map((p) => `${p[0].toUpperCase() + p.slice(1)},${p}-${stamp}@demo.test,Bulk,demopw`),
  ].join('\n');
  const bulkResult = await fetch(`${API}/participants/bulk`, {
    method: 'POST',
    headers: { ...adminHeaders, 'Content-Type': 'text/csv' },
    body: csv,
  }).then((r) => r.json());
  log(`  ✓ bulk import: ${bulkResult.created} created, ${bulkResult.skipped} skipped`);

  // ── 6. Each participant logs in and checks in ────────────────────
  // Note: Each login uses a separate auth call. The platform rate-limits
  // /auth/* to 10 req/min per IP. We use one admin token to register
  // participants in bulk and check them in via simulated participant tokens
  // produced from the registration response (skipping login).
  log('Step 6: register participants individually (returns token directly)');
  for (const p of participants) {
    const reg = await api('POST', '/auth/register', {
      body: JSON.stringify({
        name: `${p[0].toUpperCase() + p.slice(1)} Demo`,
        email: `${p}-checkin-${stamp}@demo.test`,
        team: 'DemoTeam',
        password: 'demopw',
      }),
    });
    const t = reg.token;
    await api('POST', '/attendance/checkin', {
      headers: { Authorization: `Bearer ${t}` },
      body: JSON.stringify({
        session_code: session.code,
        device_id: `${p}-phone-${stamp}`,
      }),
    });
    log(`  ✓ ${p} registered + checked in`);
    await sleep(1500); // Stagger so WS shows them one at a time
  }

  // ── 7. Wait for WS to receive all events ──────────────────────────
  log('Step 7: waiting 4s for WebSocket to drain feed');
  await sleep(4000);

  // ── 8. CSV export ─────────────────────────────────────────────────
  log('Step 8: download CSV export');
  const csvResp = await fetch(`${API}/attendance/session/${session.id}/export`, {
    headers: adminHeaders,
  });
  const csvData = await csvResp.text();
  const lineCount = csvData.split('\n').filter((l) => l.trim()).length;
  log(`  ✓ CSV downloaded: ${lineCount - 1} attendance rows`);
  console.log('\nCSV preview:');
  console.log(csvData.split('\n').slice(0, 4).join('\n'));

  // ── 9. Final stats ────────────────────────────────────────────────
  log('Step 9: final stats');
  const sessionDetail = await api('GET', `/sessions/${session.id}`, { headers: adminHeaders });
  log(`  ✓ session ${session.id} attendance_count: ${sessionDetail.attendance_count}`);

  ws.close();

  console.log();
  console.log('━'.repeat(70));
  console.log('Demo summary');
  console.log('━'.repeat(70));
  console.log(`  Event:                ${event.name} (id=${event.id})`);
  console.log(`  Session:              ${session.code} (id=${session.id})`);
  console.log(`  Participants created: ${bulkResult.created}`);
  console.log(`  Check-ins recorded:   ${sessionDetail.attendance_count}`);
  console.log(`  WS events received:   ${wsEvents.length} (1 connected + ${wsEvents.length - 1} checkins)`);
  console.log(`  CSV rows:             ${lineCount - 1}`);
  console.log('━'.repeat(70));
  process.exit(0);
}

main().catch((err) => {
  console.error('FATAL:', err.message);
  process.exit(1);
});
