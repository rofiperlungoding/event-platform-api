#!/usr/bin/env node
/**
 * One-shot stampede orchestrator. Does setup + seed + stampede in one call.
 *
 * Usage: node run-stampede.js [n] [api_url]
 *   n        — concurrent users (default 2000)
 *   api_url  — base API URL (default https://api.rofidoesthings.site)
 *
 * Env: ADMIN_EMAIL, ADMIN_PASSWORD (default admin@intrivia.test / admin123)
 */
import {Agent, setGlobalDispatcher} from 'undici';
const N = parseInt(process.argv[2] || '2000');
const API = process.argv[3] || 'https://api.rofidoesthings.site';
const ADMIN_EMAIL = process.env.ADMIN_EMAIL || 'admin@intrivia.test';
const ADMIN_PASSWORD = process.env.ADMIN_PASSWORD || 'admin123';
const RUN_ID = 'r' + Date.now().toString(36);

/* Use undici with a high-concurrency dispatcher. Node's default fetch
 * caps connections per host low, causing spurious "fetch failed" during
 * a stampede. */
setGlobalDispatcher(new Agent({
  connections: 2000,
  pipelining: 0,
  keepAliveTimeout: 10_000,
  keepAliveMaxTimeout: 60_000,
}));

console.log(`\n💥 Orchestrated stampede: ${N} concurrent\n`);
console.log(`   API:    ${API}`);
console.log(`   Run id: ${RUN_ID}\n`);

(async () => {
  /* 1. Admin login */
  const lr = await fetch(`${API}/auth/login`, {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({email: ADMIN_EMAIL, password: ADMIN_PASSWORD}),
  });
  const ld = await lr.json();
  if (!lr.ok) throw new Error('login: ' + (ld.error || lr.status));
  const token = ld.token;
  console.log('  ✓ admin logged in');

  /* 2. Seed N participants + devices */
  const t0 = Date.now();
  const sr = await fetch(`${API}/participants/seed-stamp`, {
    method: 'POST',
    headers: {'Content-Type': 'application/json', 'Authorization': `Bearer ${token}`},
    body: JSON.stringify({n: String(N), run_id: RUN_ID}),
  });
  const sd = await sr.json();
  if (!sr.ok) throw new Error('seed: ' + (sd.error || sr.status));
  console.log(`  ✓ seeded ${sd.created} participants, ${sd.devices} devices in ${(Date.now() - t0) / 1000}s`);
  console.log(`  ✓ id range: ${sd.min_id}..${sd.max_id}`);

  /* 3. Create session */
  const cs = await fetch(`${API}/sessions/create`, {
    method: 'POST',
    headers: {'Content-Type': 'application/json', 'Authorization': `Bearer ${token}`},
    body: JSON.stringify({}),
  });
  const csd = await cs.json();
  if (!cs.ok) throw new Error('session: ' + (csd.error || cs.status));
  const sessionCode = csd.code;
  console.log(`  ✓ session ${sessionCode}`);

  /* 4. Build device UUIDs from id range — no API call needed. */
  const deviceUuids = [];
  for (let id = sd.min_id; id <= sd.max_id && deviceUuids.length < N; id++) {
    deviceUuids.push(`dev-stamp-${RUN_ID}-${id}`);
  }
  console.log(`  ✓ ${deviceUuids.length} device uuids built from id range`);

  /* 5. STAMPEDE — fire all in flights of 500 to avoid local FD exhaustion. */
  console.log(`\n━━━ STAMPEDE: firing ${deviceUuids.length} parallel /attendance/quick-checkin ━━━`);
  const start = Date.now();
  const results = new Array(deviceUuids.length);
  const FLIGHT = 500;     // max concurrent in-flight fetches per batch
  const BATCH_DELAY_MS = 0;

  let nextIdx = 0;
  let active = 0;
  const fireOne = idx => {
    const t = Date.now();
    active++;
    return fetch(`${API}/attendance/quick-checkin`, {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({session_code: sessionCode, device_uuid: deviceUuids[idx]}),
    })
      .then(async r => ({ok: r.ok, status: r.status, latency: Date.now() - t,
                          body: r.ok ? null : await r.text().catch(() => '')}))
      .catch(e => ({ok: false, status: 0, latency: Date.now() - t, body: e.message}))
      .then(r => { results[idx] = r; active--; return r; });
  };

  /* Kick off the first FLIGHT, then keep topping up. */
  await new Promise(resolve => {
    const tick = () => {
      while (active < FLIGHT && nextIdx < deviceUuids.length) {
        fireOne(nextIdx++);
      }
      const done = results.filter(Boolean).length;
      if (done >= deviceUuids.length) return resolve();
      setTimeout(tick, 5);
    };
    tick();
  });
  /* Wait for any in-flight to settle */
  while (results.filter(Boolean).length < deviceUuids.length) {
    await new Promise(r => setTimeout(r, 50));
  }
  const totalMs = Date.now() - start;
  console.log(`  ✓ all settled in ${totalMs}ms\n`);

  /* 6. Report */
  const ok = results.filter(r => r.ok);
  const dup = results.filter(r => r.status === 409);
  const errors = results.filter(r => !r.ok && r.status !== 409);
  const lats = results.map(r => r.latency).sort((a, b) => a - b);
  const pct = p => lats[Math.floor(lats.length * p / 100)] || 0;
  const avg = lats.reduce((a, b) => a + b, 0) / lats.length;
  const errBy = {};
  for (const e of errors) {
    const k = `${e.status} ${(e.body || '').slice(0, 70)}`;
    errBy[k] = (errBy[k] || 0) + 1;
  }
  console.log('━━━ RESULTS ━━━');
  console.log(`  Total:        ${results.length}`);
  console.log(`  Successful:   ${ok.length}  (${(ok.length / results.length * 100).toFixed(1)}%)`);
  console.log(`  Duplicates:   ${dup.length}`);
  console.log(`  Errors:       ${errors.length}`);
  console.log(`  Throughput:   ${(results.length / (totalMs / 1000)).toFixed(0)} req/s`);
  console.log(`  Latency ms:   min=${lats[0]} avg=${avg.toFixed(0)} p50=${pct(50)} p95=${pct(95)} p99=${pct(99)} max=${lats[lats.length - 1]}`);
  if (errors.length) {
    console.log('  Errors:');
    for (const [k, v] of Object.entries(errBy)) console.log(`    ${v}x  ${k}`);
    /* Print one full error for diagnosis */
    if (errors[0]) {
      console.log('\n  First error detail:');
      console.log('    status:', errors[0].status);
      console.log('    latency:', errors[0].latency);
      console.log('    body:', errors[0].body);
    }
  }
  const goodPct = (ok.length + dup.length) / results.length * 100;
  console.log(`\n  ${goodPct >= 99.5 ? '✅ PASS' : goodPct >= 95 ? '⚠️ MARGINAL' : '❌ FAIL'}: ${goodPct.toFixed(2)}% accepted`);

  /* 7. Cleanup seeded data */
  console.log('\n→ cleaning up seeded data ...');
  await fetch(`${API}/participants/seed-cleanup`, {
    method: 'POST',
    headers: {'Content-Type': 'application/json', 'Authorization': `Bearer ${token}`},
    body: JSON.stringify({run_id: RUN_ID}),
  });
  console.log('  ✓ done\n');
})().catch(e => { console.error('Fatal:', e.message); process.exit(1); });
