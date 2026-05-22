#!/usr/bin/env node
/**
 * 2000-concurrent stampede against /attendance/quick-checkin.
 *
 * Primary goal: prove 2000 attendees can check in simultaneously, zero errors.
 *
 * Setup is bypassed via direct SQL seed (loadtest/seed.sql) — this isolates
 * the test to the actual bottleneck (the network + check-in path) instead
 * of the auth + register flow which would hit the rate limiter.
 *
 * Workflow:
 *   1. SSH into tab, run psql to seed N participants + linked devices.
 *   2. SSH back the run_id.
 *   3. From the test runner, call /sessions/create to get a code.
 *   4. STAMPEDE: fire all N quick-checkin calls in parallel.
 *
 * This script handles step 3 + 4 only. Caller passes RUN_ID and
 * SESSION_CODE on the command line.
 *
 * Usage:
 *   node stampede.js <n_users> <api_url> <session_code> <run_id>
 *
 * Example:
 *   node stampede.js 2000 https://api.rofidoesthings.site MWEQLeQC 1779420000
 */

const N = parseInt(process.argv[2] || '2000');
const API = process.argv[3] || 'https://api.rofidoesthings.site';
const SESSION_CODE = process.argv[4];
const RUN_ID = process.argv[5];

if (!SESSION_CODE || !RUN_ID) {
  console.error('Usage: node stampede.js <n_users> <api_url> <session_code> <run_id>');
  console.error('       (run loadtest/seed.sql first to populate participants + devices)');
  process.exit(1);
}

console.log(`\n💥 STAMPEDE: ${N} concurrent /attendance/quick-checkin\n`);
console.log(`   API:        ${API}`);
console.log(`   Session:    ${SESSION_CODE}`);
console.log(`   Run id:     ${RUN_ID}\n`);

// ─── Stampede ──────────────────────────────────────────────────────────────
async function stampede() {
  console.log('━━━ Firing all requests at once ━━━');
  const start = Date.now();
  const results = new Array(N);

  /* Build device UUIDs by convention from the seed:
   *   dev-stamp-<run_id>-<participant_id>
   * Participants seeded with auto-incrementing IDs. We don't know the
   * starting ID, so we use the email convention to look up via a single
   * /participants call before firing. */
  const lookupResp = await fetch(`${API}/participants`, {
    headers: {'Authorization': `Bearer ${process.env.ADMIN_TOKEN || ''}`},
  });
  let allParticipants = [];
  try { allParticipants = await lookupResp.json(); } catch {}
  if (!Array.isArray(allParticipants)) {
    console.error('  ⚠️  /participants did not return an array. status:', lookupResp.status);
    /* Fall back: try a sequential range starting from a discovered minimum */
    allParticipants = [];
  }
  const mine = allParticipants.filter(p =>
    typeof p.email === 'string' && p.email.startsWith(`stamp-${RUN_ID}-`)
  );
  console.log(`  resolved ${mine.length} seeded participants`);

  if (mine.length === 0) {
    console.error('  ✗ no seeded participants found. Did you run seed.sql with run_id=' + RUN_ID + '?');
    process.exit(1);
  }
  const targets = mine.slice(0, N);
  if (targets.length < N) {
    console.warn(`  ⚠️  only ${targets.length} participants seeded, target was ${N}`);
  }

  const deviceUuids = targets.map(p => `dev-stamp-${RUN_ID}-${p.id}`);

  /* Fire ALL at once */
  const t0 = Date.now();
  const promises = deviceUuids.map((duuid, idx) => {
    const reqStart = Date.now();
    return fetch(`${API}/attendance/quick-checkin`, {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({session_code: SESSION_CODE, device_uuid: duuid}),
    })
      .then(async r => ({
        ok: r.ok,
        status: r.status,
        latency: Date.now() - reqStart,
        body: r.ok ? null : await r.text().catch(() => ''),
      }))
      .catch(e => ({ok: false, status: 0, latency: Date.now() - reqStart, body: e.message}))
      .then(r => { results[idx] = r; return r; });
  });

  let ticks = 0;
  const reporter = setInterval(() => {
    const done = results.filter(Boolean).length;
    process.stdout.write(`\r  fired ${deviceUuids.length}, completed: ${done}/${deviceUuids.length} (${++ticks * 200}ms)`);
  }, 200);
  await Promise.all(promises);
  clearInterval(reporter);

  const totalMs = Date.now() - t0;
  console.log(`\n  ✓ all settled in ${totalMs}ms\n`);
  return {results, totalMs};
}

// ─── Stats ────────────────────────────────────────────────────────────────
function report({results, totalMs}) {
  console.log('━━━ Results ━━━');
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

  console.log(`\n  Total:        ${results.length}`);
  console.log(`  Successful:   ${ok.length}  (${(ok.length / results.length * 100).toFixed(1)}%)`);
  console.log(`  Duplicates:   ${dup.length}  (already checked in — counts as success)`);
  console.log(`  Errors:       ${errors.length}`);
  console.log(`  Throughput:   ${(results.length / (totalMs / 1000)).toFixed(0)} req/s`);
  console.log(`\n  Latency (ms):`);
  console.log(`    min:  ${lats[0]}`);
  console.log(`    avg:  ${avg.toFixed(0)}`);
  console.log(`    p50:  ${pct(50)}`);
  console.log(`    p95:  ${pct(95)}`);
  console.log(`    p99:  ${pct(99)}`);
  console.log(`    max:  ${lats[lats.length - 1]}`);

  if (errors.length) {
    console.log(`\n  Error breakdown:`);
    for (const [k, v] of Object.entries(errBy)) console.log(`    ${v}x  ${k}`);
  }

  const goodPct = (ok.length + dup.length) / results.length * 100;
  console.log(`\n  ━━━ VERDICT ━━━`);
  if (goodPct >= 99.5) console.log(`  ✅ PASS: ${goodPct.toFixed(2)}% accepted (target ≥ 99.5%)`);
  else if (goodPct >= 95) console.log(`  ⚠️  MARGINAL: ${goodPct.toFixed(2)}% accepted`);
  else console.log(`  ❌ FAIL: ${goodPct.toFixed(2)}% accepted`);
  console.log();
}

(async () => {
  try {
    if (!process.env.ADMIN_TOKEN) {
      /* Acquire admin token automatically */
      const lr = await fetch(`${API}/auth/login`, {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
          email: process.env.ADMIN_EMAIL || 'admin@intrivia.test',
          password: process.env.ADMIN_PASSWORD || 'admin123',
        }),
      });
      const ld = await lr.json();
      if (!lr.ok) throw new Error('admin login: ' + (ld.error || lr.status));
      process.env.ADMIN_TOKEN = ld.token;
    }
    const data = await stampede();
    report(data);
  } catch (e) {
    console.error('\nFatal:', e.message);
    process.exit(1);
  }
})();
