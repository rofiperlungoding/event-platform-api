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

/* Round 10 fix (audit item 286): validate argv. A typo like
 * `node run-stampede.js abc` previously made N = NaN and the
 * loop iterated forever. */
function parseInt32(s, fallback, name) {
  const n = parseInt(s, 10);
  if (!Number.isFinite(n) || n <= 0 || n > 100000) {
    console.error(`Invalid ${name}: ${s}. Using fallback ${fallback}.`);
    return fallback;
  }
  return n;
}

const N = parseInt32(process.argv[2] || '2000', 2000, 'n');
const API = process.argv[3] || 'https://api.rofidoesthings.site';
const ADMIN_EMAIL = process.env.ADMIN_EMAIL || 'admin@intrivia.test';
const ADMIN_PASSWORD = process.env.ADMIN_PASSWORD || 'admin123';
const RUN_ID = 'r' + Date.now().toString(36);

/* Round 10 fix (audit item 290): redact any error body before logging
 * so a server-side crash dump that happened to capture user data
 * does not leak through console output. */
function redact(body) {
  if (!body) return '';
  const s = String(body);
  return s
    .replace(/("password"\s*:\s*")[^"]+(")/g, '$1***$2')
    .replace(/(Bearer\s+)[A-Za-z0-9_:.-]+/g, '$1***')
    .slice(0, 200);
}

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

/* Round 10 fix (audit item 288): trap SIGINT so a Ctrl-C in the
 * middle of a stampede still cleans up the seeded data. Without
 * this the operator was left with thousands of orphan participants
 * tagged `dev-stamp-...` that later showed up in the dashboard. */
let cleanupToken = null;
process.on('SIGINT', async () => {
  console.log('\n⚠ SIGINT received — running cleanup before exit');
  if (cleanupToken) {
    try {
      await fetch(`${API}/participants/seed-cleanup`, {
        method: 'POST',
        headers: {'Content-Type': 'application/json', 'Authorization': `Bearer ${cleanupToken}`},
        body: JSON.stringify({run_id: RUN_ID}),
      });
      console.log('  ✓ cleanup ok');
    } catch (e) { console.error('  cleanup failed:', e.message); }
  }
  process.exit(130);
});

(async () => {
  /* 1. Admin login */
  const lr = await fetch(`${API}/auth/login`, {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({email: ADMIN_EMAIL, password: ADMIN_PASSWORD}),
  });
  const ld = await lr.json();
  if (!lr.ok) throw new Error('login: ' + (ld.error || lr.status));
  const token = ld.token;
  cleanupToken = token;     /* arm SIGINT trap (round 10) */
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

  /* Per-request settings: short timeout so a stuck connection retries
   * fast instead of holding a socket for 30s. */
  const PER_TRY_TIMEOUT_MS = 6000;
  const MAX_TRIES = 5;

  /* Smart-retry single check-in: short timeout, jittered exponential
   * backoff. A single device's check-in is idempotent server-side
   * (UNIQUE constraint on participant_id+session_id), so retries can
   * never cause duplicate attendance — they either succeed, return 409
   * (already checked in, treat as success), or surface a real error. */
  const checkinOne = async (duuid, idx) => {
    const t0 = Date.now();
    let lastBody = '';
    let lastStatus = 0;
    for (let attempt = 1; attempt <= MAX_TRIES; attempt++) {
      const ctl = new AbortController();
      const timer = setTimeout(() => ctl.abort(), PER_TRY_TIMEOUT_MS);
      try {
        const r = await fetch(`${API}/attendance/quick-checkin`, {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({session_code: sessionCode, device_uuid: duuid}),
          signal: ctl.signal,
        });
        clearTimeout(timer);
        if (r.ok || r.status === 409) {
          results[idx] = {ok: r.ok, status: r.status, latency: Date.now() - t0,
                          body: null, attempts: attempt};
          return;
        }
        lastStatus = r.status;
        lastBody = await r.text().catch(() => '');
        /* 4xx other than 409 are deterministic — no point retrying */
        if (r.status >= 400 && r.status < 500 && r.status !== 408) break;
      } catch (e) {
        clearTimeout(timer);
        lastBody = e.message || 'fetch failed';
      }
      /* Jittered backoff: 50, 100, 200, 400 ms ± 30 % */
      const base = 50 * Math.pow(2, attempt - 1);
      const delay = base + Math.floor(Math.random() * base * 0.6);
      await new Promise(r => setTimeout(r, delay));
    }
    results[idx] = {ok: false, status: lastStatus, latency: Date.now() - t0,
                    body: lastBody, attempts: MAX_TRIES};
  };

  let nextIdx = 0;
  let active = 0;
  const fireOne = idx => {
    active++;
    return checkinOne(deviceUuids[idx], idx).finally(() => { active--; });
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
  /* Round 10 fix (audit item 289): handle empty results array. */
  const pct = p => lats.length ? (lats[Math.floor(lats.length * p / 100)] || 0) : 0;
  const avg = lats.length ? lats.reduce((a, b) => a + b, 0) / lats.length : 0;
  const errBy = {};
  for (const e of errors) {
    const k = `${e.status} ${redact(e.body).slice(0, 70)}`;
    errBy[k] = (errBy[k] || 0) + 1;
  }
  console.log('━━━ RESULTS ━━━');
  console.log(`  Total:        ${results.length}`);
  console.log(`  Successful:   ${ok.length}  (${(ok.length / results.length * 100).toFixed(1)}%)`);
  console.log(`  Duplicates:   ${dup.length}`);
  console.log(`  Errors:       ${errors.length}`);
  console.log(`  Throughput:   ${(results.length / (totalMs / 1000)).toFixed(0)} req/s`);
  console.log(`  Latency ms:   min=${lats[0]} avg=${avg.toFixed(0)} p50=${pct(50)} p95=${pct(95)} p99=${pct(99)} max=${lats[lats.length - 1]}`);

  /* Retry distribution (idempotent retries are how we hit 100 %) */
  const byAttempt = {};
  for (const r of results) {
    const a = r.attempts || 1;
    byAttempt[a] = (byAttempt[a] || 0) + 1;
  }
  console.log(`  Attempts:     ` + Object.entries(byAttempt).sort().map(([k, v]) => `${k}x:${v}`).join('  '));
  if (errors.length) {
    console.log('  Errors:');
    for (const [k, v] of Object.entries(errBy)) console.log(`    ${v}x  ${k}`);
    /* Print one full error for diagnosis */
    if (errors[0]) {
      console.log('\n  First error detail:');
      console.log('    status:', errors[0].status);
      console.log('    latency:', errors[0].latency);
      console.log('    body:', redact(errors[0].body));
    }
  }
  const goodPct = (ok.length + dup.length) / results.length * 100;
  console.log(`\n  ${goodPct >= 99.5 ? '✅ PASS' : goodPct >= 95 ? '⚠️ MARGINAL' : '❌ FAIL'}: ${goodPct.toFixed(2)}% accepted`);

  /* 7. Cleanup seeded data (skip with NO_CLEANUP=1 to keep data visible
   *    in the operational dashboard for inspection) */
  if (process.env.NO_CLEANUP === '1') {
    console.log('\n→ NO_CLEANUP=1 — keeping seeded data for inspection');
    console.log(`   To clean up later:  curl -X POST ${API}/participants/seed-cleanup \\`);
    console.log(`                          -H "Authorization: Bearer <admin>" \\`);
    console.log(`                          -d '{"run_id":"${RUN_ID}"}'`);
    console.log();
    return;
  }
  console.log('\n→ cleaning up seeded data ...');
  await fetch(`${API}/participants/seed-cleanup`, {
    method: 'POST',
    headers: {'Content-Type': 'application/json', 'Authorization': `Bearer ${token}`},
    body: JSON.stringify({run_id: RUN_ID}),
  });
  console.log('  ✓ done\n');
})().catch(e => { console.error('Fatal:', e.message); process.exit(1); });
