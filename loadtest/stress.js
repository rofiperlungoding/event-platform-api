#!/usr/bin/env node
/**
 * Load test: simulate N participants checking in concurrently.
 *
 * Usage:
 *   node stress.js <n_users> <api_url> <session_code> <admin_token>
 *
 * Example:
 *   node stress.js 2000 https://api.rofidoesthings.site ABC12345 <token>
 */

const N = parseInt(process.argv[2] || '100');
const API = process.argv[3] || 'https://api.rofidoesthings.site';
const SESSION_CODE = process.argv[4];
const ADMIN_TOKEN = process.argv[5];

if (!SESSION_CODE || !ADMIN_TOKEN) {
  console.error('Usage: node stress.js <n_users> <api_url> <session_code> <admin_token>');
  process.exit(1);
}

console.log(`\n🔥 Load test: ${N} concurrent check-ins\n`);
console.log(`   API:     ${API}`);
console.log(`   Session: ${SESSION_CODE}\n`);

// ─── Phase 1: Setup users (sequential, fast bulk insert) ──────────────────
async function setup() {
  console.log('━━━ Phase 1: Creating users + getting tokens ━━━');
  const users = [];
  const t0 = Date.now();
  const batch = 50; // parallel registrations

  for (let i = 0; i < N; i += batch) {
    const tasks = [];
    for (let j = i; j < Math.min(i + batch, N); j++) {
      const email = `loadtest${j}_${Date.now()}@test.local`;
      tasks.push(
        fetch(`${API}/auth/register`, {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({
            name: `Load User ${j}`,
            email,
            team: `Team ${j % 10}`,
            password: 'pw123',
          }),
        })
          .then(r => r.json())
          .then(d => {
            if (d.token) users.push({token: d.token, id: d.participant.id});
            else console.error(`  user ${j} register failed:`, d.error);
          })
          .catch(e => console.error(`  user ${j} error:`, e.message))
      );
    }
    await Promise.all(tasks);
    process.stdout.write(`\r  registered: ${users.length}/${N}`);
  }
  console.log(`\n  ✓ ${users.length} users ready in ${(Date.now() - t0) / 1000}s\n`);
  return users;
}

// ─── Phase 2: Concurrent check-in stampede ─────────────────────────────────
async function stampede(users) {
  console.log('━━━ Phase 2: Concurrent check-in stampede ━━━');
  const start = Date.now();
  const results = [];

  // Fire all at once
  const promises = users.map((u, idx) => {
    const reqStart = Date.now();
    return fetch(`${API}/attendance/checkin`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'Authorization': `Bearer ${u.token}`,
      },
      body: JSON.stringify({
        session_code: SESSION_CODE,
        device_id: `loadtest-device-${idx}`,
      }),
    })
      .then(async r => ({
        ok: r.ok,
        status: r.status,
        latency: Date.now() - reqStart,
        body: r.ok ? null : await r.text().catch(() => ''),
      }))
      .catch(e => ({
        ok: false,
        status: 0,
        latency: Date.now() - reqStart,
        body: e.message,
      }));
  });

  // Progress reporter
  let done = 0;
  const reporter = setInterval(() => {
    process.stdout.write(`\r  done: ${done}/${users.length}`);
  }, 200);

  for (const p of promises) {
    p.then(r => { results.push(r); done++; });
  }
  await Promise.all(promises);
  clearInterval(reporter);

  const totalMs = Date.now() - start;
  console.log(`\n  ✓ all done in ${totalMs}ms\n`);

  return {results, totalMs};
}

// ─── Phase 3: Stats ────────────────────────────────────────────────────────
function report({results, totalMs}) {
  console.log('━━━ Phase 3: Results ━━━');

  const ok = results.filter(r => r.ok);
  const errors = results.filter(r => !r.ok);
  const latencies = results.map(r => r.latency).sort((a, b) => a - b);

  const pct = p => latencies[Math.floor(latencies.length * p / 100)] || 0;
  const avg = latencies.reduce((a, b) => a + b, 0) / latencies.length;

  // Error breakdown by status
  const errBy = {};
  for (const e of errors) {
    const key = `${e.status} ${(e.body || '').slice(0, 60)}`;
    errBy[key] = (errBy[key] || 0) + 1;
  }

  console.log(`\n  Total requests: ${results.length}`);
  console.log(`  Successful:     ${ok.length}  (${(ok.length / results.length * 100).toFixed(1)}%)`);
  console.log(`  Failed:         ${errors.length}`);
  console.log(`  Throughput:     ${(results.length / (totalMs / 1000)).toFixed(0)} req/s`);
  console.log(`\n  Latency (ms):`);
  console.log(`    min:  ${latencies[0]}`);
  console.log(`    avg:  ${avg.toFixed(0)}`);
  console.log(`    p50:  ${pct(50)}`);
  console.log(`    p95:  ${pct(95)}`);
  console.log(`    p99:  ${pct(99)}`);
  console.log(`    max:  ${latencies[latencies.length - 1]}`);

  if (errors.length) {
    console.log(`\n  Error breakdown:`);
    for (const [k, v] of Object.entries(errBy)) {
      console.log(`    ${v}x  ${k}`);
    }
  }
  console.log();
}

// ─── Run ───────────────────────────────────────────────────────────────────
(async () => {
  try {
    const users = await setup();
    if (!users.length) { console.error('No users created, abort.'); return; }
    const data = await stampede(users);
    report(data);
  } catch (e) {
    console.error('Fatal:', e);
  }
})();
