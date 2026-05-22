/* Event Platform Console — vanilla JS, no build, no deps */
const API = 'https://api.rofidoesthings.site';
const REFRESH_MS = 8000;

// ─── Helpers ────────────────────────────────────────────────────────────────
const $ = (sel) => document.querySelector(sel);
const $$ = (sel) => document.querySelectorAll(sel);

const fmtBytes = (b) => {
  if (b == null || isNaN(b)) return '—';
  const u = ['B', 'KB', 'MB', 'GB', 'TB'];
  let i = 0; let n = Number(b);
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return `${n.toFixed(n >= 100 ? 0 : n >= 10 ? 1 : 2)} ${u[i]}`;
};

const fmtUptime = (sec) => {
  if (sec == null) return '—';
  const d = Math.floor(sec / 86400);
  const h = Math.floor((sec % 86400) / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = Math.floor(sec % 60);
  if (d) return `${d}d ${h}h`;
  if (h) return `${h}h ${m}m`;
  if (m) return `${m}m ${s}s`;
  return `${s}s`;
};

const fmtTime = (iso) => {
  const d = new Date(iso);
  return d.toLocaleString(undefined, { hour12: false });
};

const fmtRel = (iso) => {
  const d = new Date(iso).getTime();
  const diff = (Date.now() - d) / 1000;
  if (diff < 60) return `${Math.floor(diff)}s ago`;
  if (diff < 3600) return `${Math.floor(diff / 60)}m ago`;
  if (diff < 86400) return `${Math.floor(diff / 3600)}h ago`;
  return `${Math.floor(diff / 86400)}d ago`;
};

async function fetchJson(path) {
  const res = await fetch(`${API}${path}`, { cache: 'no-store' });
  if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
  return res.json();
}

// ─── Tabs ───────────────────────────────────────────────────────────────────
$$('.nav-item').forEach((el) => {
  el.addEventListener('click', () => {
    const tab = el.dataset.tab;
    $$('.nav-item').forEach((n) => n.classList.toggle('active', n === el));
    $$('.tab-panel').forEach((p) =>
      p.classList.toggle('active', p.id === `tab-${tab}`),
    );
    $('#page-title').textContent = el.textContent.trim();
  });
});

// ─── Status pill ────────────────────────────────────────────────────────────
function setStatus(ok, text) {
  $('#status-dot').className = `status-dot ${ok ? 'ok' : 'error'}`;
  $('#status-text').textContent = text;
}

// ─── Renderers ──────────────────────────────────────────────────────────────
function renderHealth(d) {
  $('#kpi-status').textContent = d.status === 'healthy' ? 'Healthy' : 'Degraded';
  $('#kpi-status-sub').textContent = `node ${d.node_version} · v${d.version}`;
  $('#kpi-db-latency').textContent = d.checks.database.latency_ms;
  $('#kpi-uptime').textContent = fmtUptime(d.uptime_seconds);

  const badge = $('#health-badge');
  badge.textContent = d.status;
  badge.className = `badge ${d.status === 'healthy' ? 'ok' : 'err'}`;

  const rows = Object.entries(d.checks).map(([name, c]) => `
    <tr>
      <td>${name}</td>
      <td><span class="status-row">
        <span class="dot ${c.status === 'ok' ? '' : 'err'}"></span>
        ${c.status}
      </span></td>
      <td class="num">${c.latency_ms} ms</td>
    </tr>
  `).join('');
  $('#health-table').innerHTML = rows;

  $('#meta-table').innerHTML = `
    <tr><td>Service</td><td>event-platform-api</td></tr>
    <tr><td>Version</td><td>${d.version}</td></tr>
    <tr><td>Node.js</td><td>${d.node_version}</td></tr>
    <tr><td>Uptime</td><td>${fmtUptime(d.uptime_seconds)}</td></tr>
    <tr><td>Last check</td><td>${fmtTime(d.timestamp)}</td></tr>
  `;
}

function renderSystem(d) {
  // Region info in topbar
  $('#region-text').textContent = `tabserve · ${d.platform}-${d.arch} · ${d.hostname}`;

  $('#sys-cores').textContent = d.cpu.cores || '—';
  $('#sys-cpu-model').textContent = d.cpu.model && d.cpu.model !== 'unknown'
    ? d.cpu.model.slice(0, 32) : `${d.platform}/${d.arch}`;

  $('#sys-load').textContent = d.cpu.load_avg['1m'].toFixed(2);
  $('#sys-load-detail').textContent =
    `5m ${d.cpu.load_avg['5m'].toFixed(2)} · 15m ${d.cpu.load_avg['15m'].toFixed(2)}`;

  $('#sys-mem-pct').textContent = d.memory.used_percent.toFixed(1);
  $('#sys-mem-detail').textContent =
    `${fmtBytes(d.memory.used_bytes)} of ${fmtBytes(d.memory.total_bytes)}`;

  // Memory bar
  const usedPct = d.memory.used_percent;
  $('#mem-bar .seg-used').style.width = `${usedPct}%`;
  $('#mem-bar .seg-free').style.width = `${100 - usedPct}%`;

  $('#mem-table').innerHTML = `
    <tr><td>Total</td><td>${fmtBytes(d.memory.total_bytes)}</td></tr>
    <tr><td>Used</td><td>${fmtBytes(d.memory.used_bytes)} (${usedPct.toFixed(1)}%)</td></tr>
    <tr><td>Free</td><td>${fmtBytes(d.memory.free_bytes)}</td></tr>
    <tr><td>System uptime</td><td>${fmtUptime(d.uptime.system_seconds)}</td></tr>
  `;

  $('#proc-table').innerHTML = `
    <tr><td>RSS</td><td>${fmtBytes(d.process.rss_bytes)}</td></tr>
    <tr><td>Heap total</td><td>${fmtBytes(d.process.heap_total_bytes)}</td></tr>
    <tr><td>Heap used</td><td>${fmtBytes(d.process.heap_used_bytes)}</td></tr>
    <tr><td>External</td><td>${fmtBytes(d.process.external_bytes)}</td></tr>
    <tr><td>Process uptime</td><td>${fmtUptime(d.uptime.process_seconds)}</td></tr>
  `;
}

function renderDb(d) {
  $('#db-version').textContent = d.database ? d.database.version : (d.database_version || '—');
  $('#db-size').textContent = fmtBytes(d.database ? d.database.size_bytes : d.database_size_bytes);
  const tables = d.tables || [];
  $('#db-tables-count').textContent = tables.length;

  $('#db-tables-body').innerHTML = tables.map(t => `
    <tr>
      <td><code>${t.name}</code></td>
      <td class="num">${(t.row_count ?? t.live_tuples ?? 0).toLocaleString()}</td>
      <td class="num">${fmtBytes(t.size_bytes ?? 0)}</td>
    </tr>
  `).join('') || `<tr><td colspan="3" class="muted">no tables</td></tr>`;
}

function renderParticipantStats(d) {
  $('#kpi-participants').textContent = d.total;
  $('#kpi-participants-sub').textContent =
    d.by_team.length ? `${d.by_team.length} team${d.by_team.length > 1 ? 's' : ''}` : 'no teams';

  $('#part-total').textContent = d.total;
  $('#part-teams-count').textContent = d.by_team.length;

  if (d.by_team.length) {
    const top = d.by_team[0];
    $('#part-top-team').textContent = top.team;
    $('#part-top-team-sub').textContent = `${top.count} participant${top.count > 1 ? 's' : ''}`;
  } else {
    $('#part-top-team').textContent = '—';
    $('#part-top-team-sub').textContent = 'no data';
  }

  $('#team-body').innerHTML = d.by_team.map(t => `
    <tr><td>${escapeHtml(t.team)}</td><td class="num">${t.count}</td></tr>
  `).join('') || `<tr><td colspan="2" class="muted">no teams</td></tr>`;

  $('#recent-body').innerHTML = (d.recent_5 || d.recent || []).map(p => `
    <tr>
      <td>${escapeHtml(p.name)}</td>
      <td>${escapeHtml(p.team)}</td>
      <td class="muted small">${fmtRel(p.createdAt)}</td>
    </tr>
  `).join('') || `<tr><td colspan="3" class="muted">no registrations yet</td></tr>`;
}

function renderAllParticipants(list) {
  $('#all-count').textContent = `${list.length} record${list.length === 1 ? '' : 's'}`;
  $('#all-body').innerHTML = list.length ? list.map(p => `
    <tr>
      <td class="num">${p.id}</td>
      <td>${escapeHtml(p.name)}</td>
      <td><code>${escapeHtml(p.email)}</code></td>
      <td>${escapeHtml(p.team)}</td>
      <td class="muted small">${fmtTime(p.createdAt)}</td>
      <td><button class="btn-del" data-id="${p.id}">Delete</button></td>
    </tr>
  `).join('') : `<tr><td colspan="6" class="muted">no participants</td></tr>`;

  $$('.btn-del').forEach(b => b.addEventListener('click', async (e) => {
    const id = e.target.dataset.id;
    if (!confirm(`Delete participant #${id}?`)) return;
    try {
      const res = await fetch(`${API}/participants/${id}`, { method: 'DELETE' });
      if (!res.ok && res.status !== 204) throw new Error(`${res.status}`);
      refresh();
    } catch (err) { alert(`Delete failed: ${err.message}`); }
  }));
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[c]));
}

// ─── Main refresh ──────────────────────────────────────────────────────────
async function refresh() {
  const t0 = Date.now();
  try {
    const [health, sys, dbStats, partStats, partList] = await Promise.all([
      fetchJson('/health/detailed'),
      fetchJson('/system'),
      fetchJson('/stats/database'),
      fetchJson('/stats/participants'),
      fetchJson('/participants'),
    ]);
    renderHealth(health);
    renderSystem(sys);
    renderDb(dbStats);
    renderParticipantStats(partStats);
    renderAllParticipants(partList);
    setStatus(true, `online · ${Date.now() - t0}ms`);
    $('#last-update').textContent = `Updated ${new Date().toLocaleTimeString(undefined, { hour12: false })}`;
  } catch (err) {
    setStatus(false, 'offline');
    $('#last-update').textContent = `Error: ${err.message}`;
    console.error(err);
  }
}

$('#btn-refresh').addEventListener('click', refresh);
$('#endpoint-text').textContent = new URL(API).host;

refresh();
setInterval(refresh, REFRESH_MS);
