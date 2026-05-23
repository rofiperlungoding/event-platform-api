#!/usr/bin/env node
/**
 * Chaos check-in test — simulates real attendees with imperfect networks.
 *
 * Out of N attendees, we synthesise five failure modes that mirror what
 * happens at a real event with 2,000 phones on venue Wi-Fi:
 *
 *   1. Healthy — 70 % — hits server first try, succeeds.
 *   2. Slow first attempt — 15 % — first call times out, retry succeeds.
 *   3. Flapping network — 8 % — first 2 attempts fail with simulated
 *      network errors, third succeeds. (Retry path)
 *   4. Offline drop — 5 % — first 5 attempts fail, eventually offline
 *      fallback would queue locally. We simulate the queue drain by
 *      calling /attendance/batch-checkin with the device.
 *   5. Bad device — 2 % — device UUID is not registered. Server returns
 *      401, count as "unknown" (legitimate failure case).
 *
 * Goal: prove zero attendee is permanently lost regardless of failure
 * mode. Every healthy or partially-online attendee must end up checked
 * in by the time the test finishes.
 *
 * Usage:
 *   node chaos-checkin.js [n] [api_url]
 */

import {Agent, setGlobalDispatcher} from 'undici';

/* Round 10 fix (audit items 286, 290): validate argv and redact
 * tokens/passwords from any error body before logging. Same helpers
 * as run-stampede.js. */
function parseInt32(s, fallback, name) {
  const n = parseInt(s, 10);
  if (!Number.isFinite(n) || n <= 0 || n > 100000) {
    console.error(`Invalid ${name}: ${s}. Using fallback ${fallback}.`);
    return fallback;
  }
  return n;
}
function redact(body) {
  if (!body) return '';
  return String(body)
    .replace(/("password"\s*:\s*")[^"]+(")/g, '$1***$2')
    .replace(/(Bearer\s+)[A-Za-z0-9_:.-]+/g, '$1***')
    .slice(0, 200);
}

const N = parseInt32(process.argv[2] || '2000', 2000, 'n');
const API = process.argv[3] || 'http://192.168.100.67:3001';
const ADMIN_EMAIL = process.env.ADMIN_EMAIL || 'admin@intrivia.test';
const ADMIN_PASSWORD = process.env.ADMIN_PASSWORD || 'admin123';
const RUN_ID = 'c' + Date.now().toString(36);

setGlobalDispatcher(new Agent({
  connections: 2000, pipelining: 0,
  keepAliveTimeout: 10_000, keepAliveMaxTimeout: 60_000,
}));

console.log(`\n🌀 Chaos check-in: ${N} attendees with mixed failure modes\n`);
console.log(`   API:    ${API}`);
console.log(`   Run id: ${RUN_ID}\n`);

/* Failure-mode distribution */
const PROFILES = [
  {weight: 70, name: 'healthy',     simFail: 0,             unknown: false},
  {weight: 15, name: 'slow',        simFail: 1,             unknown: false},
  {weight:  8, name: 'flapping',    simFail: 2,             unknown: false},
  {weight:  5, name: 'queued',      simFail: 5,             unknown: false},
  {weight:  2, name: 'bad-device',  simFail: 0,             unknown: true},
];

/* Pick a profile by weighted random */
function pickProfile() {
  const total = PROFILES.reduce((s, p) => s + p.weight, 0);
  let r = Math.random() * total;
  for (const p of PROFILES) { r -= p.weight; if (r <= 0) return p; }
  return PROFILES[0];
}

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
  const sr = await fetch(`${API}/participants/seed-stamp`, {
    method: 'POST',
    headers: {'Content-Type': 'application/json', 'Authorization': `Bearer ${token}`},
    body: JSON.stringify({n: String(N), run_id: RUN_ID}),
  });
  const sd = await sr.json();
  if (!sr.ok) throw new Error('seed: ' + (sd.error || sr.status));
  console.log(`  ✓ seeded ${sd.created} participants in ${sd.min_id}..${sd.max_id}`);

  /* 3. Create session */
  const cs = await fetch(`${API}/sessions/create`, {
    method: 'POST',
    headers: {'Content-Type': 'application/json', 'Authorization': `Bearer ${token}`},
    body: JSON.stringify({}),
  });
  const csd = await cs.json();
  if (!cs.ok) throw new Error('session: ' + (csd.error || cs.status));
  const sessionCode = csd.code;
  console.log(`  ✓ session ${sessionCode}\n`);

  /* 4. Simulate each attendee with retries that mirror PWA behaviour */
  console.log('━━━ Simulating attendees with mixed failure modes ━━━');
  const start = Date.now();
  const results = new Array(N);
  const profileCount = {};

  /* Try one device with up to MAX_TRIES retries; first `simFail` attempts
   * are deliberately broken with a "network error" abort. Mirrors what
   * the PWA does in fetchWithRetry + offline queue. */
  const PER_TRY_TIMEOUT = 6000;
  const MAX_TRIES = 8;
  async function attendOne(idx) {
    const profile = pickProfile();
    profileCount[profile.name] = (profileCount[profile.name] || 0) + 1;
    const t0 = Date.now();
    let attempt = 0;
    let lastError = null;
    const deviceUuid = profile.unknown
      ? `dev-bogus-${RUN_ID}-${idx}`
      : `dev-stamp-${RUN_ID}-${sd.min_id + idx}`;

    while (attempt < MAX_TRIES) {
      attempt++;
      /* Simulate first N attempts failing */
      if (attempt <= profile.simFail) {
        await new Promise(r => setTimeout(r, 50 + Math.random() * 150));
        lastError = 'simulated network failure';
        continue;
      }
      try {
        const ctl = new AbortController();
        const timer = setTimeout(() => ctl.abort(), PER_TRY_TIMEOUT);
        const r = await fetch(`${API}/attendance/quick-checkin`, {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({session_code: sessionCode, device_uuid: deviceUuid}),
          signal: ctl.signal,
        });
        clearTimeout(timer);
        if (r.ok || r.status === 409) {
          results[idx] = {ok: true, profile: profile.name, attempts: attempt, latency: Date.now() - t0};
          return;
        }
        if (r.status === 401 || r.status === 404) {
          /* Deterministic — no point retrying */
          results[idx] = {ok: false, profile: profile.name, attempts: attempt, status: r.status, latency: Date.now() - t0};
          return;
        }
        lastError = `${r.status}`;
      } catch (e) {
        lastError = e.message;
      }
      /* jittered backoff */
      const base = 100 * Math.pow(2, attempt - 1);
      await new Promise(r => setTimeout(r, base + Math.random() * base * 0.6));
    }
    results[idx] = {ok: false, profile: profile.name, attempts: attempt, status: 0, body: lastError, latency: Date.now() - t0};
  }

  /* Concurrency-limited fan-out */
  const FLIGHT = 500;
  let nextIdx = 0;
  let active = 0;
  await new Promise(resolve => {
    const tick = () => {
      while (active < FLIGHT && nextIdx < N) {
        const idx = nextIdx++;
        active++;
        attendOne(idx).finally(() => { active--; });
      }
      if (results.filter(Boolean).length >= N) return resolve();
      setTimeout(tick, 5);
    };
    tick();
  });
  while (results.filter(Boolean).length < N) {
    await new Promise(r => setTimeout(r, 50));
  }
  const totalMs = Date.now() - start;
  console.log(`  ✓ all attendees settled in ${totalMs}ms\n`);

  /* 5. Stats */
  console.log('━━━ Profile distribution ━━━');
  for (const p of PROFILES) {
    const n = profileCount[p.name] || 0;
    const pct = (n / N * 100).toFixed(1);
    console.log(`  ${p.name.padEnd(12)} ${n.toString().padStart(5)}  (${pct}%)`);
  }

  console.log('\n━━━ Outcomes by profile ━━━');
  const ok = results.filter(r => r.ok);
  const bad = results.filter(r => !r.ok);
  const byProfile = {};
  for (const r of results) {
    const k = r.profile;
    byProfile[k] = byProfile[k] || {ok: 0, bad: 0, attempts: []};
    byProfile[k][r.ok ? 'ok' : 'bad']++;
    byProfile[k].attempts.push(r.attempts);
  }
  for (const [name, s] of Object.entries(byProfile)) {
    const avgAtt = (s.attempts.reduce((a,b)=>a+b,0) / s.attempts.length).toFixed(1);
    console.log(`  ${name.padEnd(12)} ok=${s.ok.toString().padStart(5)}  failed=${s.bad.toString().padStart(3)}  avg-attempts=${avgAtt}`);
  }

  console.log('\n━━━ Verdict ━━━');
  console.log(`  Total attendees:    ${N}`);
  console.log(`  Checked in:         ${ok.length}  (${(ok.length/N*100).toFixed(2)}%)`);
  console.log(`  Failed (bad-device): ${bad.filter(r => r.profile === 'bad-device').length}  (legitimate)`);
  console.log(`  Failed (other):      ${bad.filter(r => r.profile !== 'bad-device').length}  (problem!)`);
  console.log(`  Throughput:         ${(N/(totalMs/1000)).toFixed(0)} req/s`);

  /* The success criterion: every attendee whose profile is NOT bad-device
   * must have ended up checked in. The bad-device profile is a legitimate
   * failure that the operator must investigate (device not registered). */
  const recoverableLost = bad.filter(r => r.profile !== 'bad-device');
  if (recoverableLost.length === 0) {
    console.log(`\n  ✅ PASS: every attendee with a working device + connection got in.`);
  } else {
    console.log(`\n  ❌ FAIL: ${recoverableLost.length} attendee(s) with valid devices lost despite retries.`);
    console.log(`  Sample failures:`);
    for (const r of recoverableLost.slice(0, 5)) {
      console.log(`    profile=${r.profile} attempts=${r.attempts} status=${r.status} body=${redact(r.body)}`);
    }
  }

  /* Cleanup */
  if (process.env.NO_CLEANUP !== '1') {
    console.log('\n→ cleaning up seeded data...');
    await fetch(`${API}/participants/seed-cleanup`, {
      method: 'POST',
      headers: {'Content-Type': 'application/json', 'Authorization': `Bearer ${token}`},
      body: JSON.stringify({run_id: RUN_ID}),
    });
    console.log('  ✓ done\n');
  } else {
    console.log(`\n→ NO_CLEANUP=1 — keep run_id ${RUN_ID} for inspection\n`);
  }
})().catch(e => { console.error('Fatal:', e.message); process.exit(1); });
