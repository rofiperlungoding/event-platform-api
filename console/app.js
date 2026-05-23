/* Event Platform Console — vanilla JS, AWS-grade redesign */
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
  if (!iso) return '—';
  const d = new Date(iso);
  if (isNaN(d.getTime())) return '—';
  return d.toLocaleString(undefined, { hour12: false });
};

const fmtRel = (iso) => {
  const d = new Date(iso).getTime();
  if (isNaN(d)) return '—';
  const diff = (Date.now() - d) / 1000;
  if (diff < 60) return `${Math.floor(diff)}s ago`;
  if (diff < 3600) return `${Math.floor(diff / 60)}m ago`;
  if (diff < 86400) return `${Math.floor(diff / 3600)}h ago`;
  return `${Math.floor(diff / 86400)}d ago`;
};

/* Threshold class for usage % values */
const usageClass = (pct) => {
  if (pct >= 90) return 'high';
  if (pct >= 70) return 'warn';
  return '';
};

/* Apply status accent to a stat-card by id */
const applyStatus = (cardId, status) => {
  const el = document.getElementById(cardId);
  if (!el) return;
  el.classList.remove('status-ok', 'status-warn', 'status-error', 'status-info', 'status-accent');
  if (status) el.classList.add(`status-${status}`);
};

async function fetchJson(path, opts = {}) {
  /* Round 7 fix (audit item 228): every console fetch now has a
   * 6-second timeout. Without it, a stuck endpoint blocks the
   * `Promise.allSettled` for the full duration of whatever the
   * browser's default timeout is (60 s on Chromium) and the
   * dashboard appears frozen. AbortController fires the rejection
   * cleanly so the per-section error-state renderer takes over. */
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), opts.timeout || 6000);
  try {
    const res = await fetch(`${API}${path}`, { cache: 'no-store', signal: ctl.signal });
    if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
    return await res.json();
  } finally {
    clearTimeout(t);
  }
}

// ─── Tabs ───────────────────────────────────────────────────────────────────
$$('.nav-item').forEach((el) => {
  el.addEventListener('click', () => {
    const tab = el.dataset.tab;
    $$('.nav-item').forEach((n) => {
      const isActive = n === el;
      n.classList.toggle('active', isActive);
      /* Round 12 a11y: keep aria-selected in sync so screen readers
       * announce the right tab on switch. */
      n.setAttribute('aria-selected', isActive ? 'true' : 'false');
    });
    $$('.tab-panel').forEach((p) =>
      p.classList.toggle('active', p.id === `tab-${tab}`),
    );
    $('#page-title').textContent = el.textContent.trim();
  });
});

// ─── Status pill state machine ──────────────────────────────────────────────
let lastSuccessfulRefresh = null;

function setStatus(state, text) {
  const dot = $('#status-dot');
  const txt = $('#status-text');
  dot.className = 'status-dot ' + state;       // connecting | ok | warn | error
  txt.textContent = text;
}

/* Continuously update "Last updated: Xs ago" without re-fetching */
function tickLastUpdate() {
  if (!lastSuccessfulRefresh) return;
  $('#last-update').textContent = `Last updated: ${fmtRel(lastSuccessfulRefresh)}`;
}
setInterval(tickLastUpdate, 1000);

// ─── Renderers ──────────────────────────────────────────────────────────────
function renderHealth(d) {
  // Status KPI card with semantic accent + value
  const isHealthy = d.status === 'healthy';
  $('#kpi-status').textContent = isHealthy ? 'Healthy' : 'Degraded';
  $('#kpi-status-sub').textContent = `${d.node_version || 'native'} · v${d.version || '—'}`;
  applyStatus('kpi-card-status', isHealthy ? 'ok' : 'error');

  // DB latency
  const dbLat = d.checks?.database?.latency_ms ?? 0;
  $('#kpi-db-latency-wrap').innerHTML =
    `<span>${dbLat}</span><span class="unit">ms</span>`;
  applyStatus('kpi-card-latency', dbLat < 50 ? 'ok' : dbLat < 200 ? 'warn' : 'error');

  // Uptime KPI — service uptime is what matters (persists across deploys)
  const svcUp = d.service_uptime_seconds ?? d.uptime_seconds;
  const procUp = d.uptime_seconds;
  $('#kpi-uptime').textContent = fmtUptime(svcUp);
  $('#kpi-uptime-sub').textContent = procUp < svcUp
    ? `process: ${fmtUptime(procUp)} (last deploy)`
    : `since first deploy`;
  applyStatus('kpi-card-uptime', svcUp >= 86400 ? 'ok' : 'info');

  // Health checks badge — semantic
  const badge = $('#health-badge');
  badge.textContent = d.status;
  badge.className = `badge ${isHealthy ? 'ok' : 'err'}`;

  const rows = Object.entries(d.checks || {}).map(([name, c]) => `
    <tr>
      <td>${escapeHtml(name)}</td>
      <td><span class="status-row">
        <span class="dot ${c.status === 'ok' ? '' : 'err'}"></span>
        ${escapeHtml(c.status)}
      </span></td>
      <td class="num">${c.latency_ms} ms</td>
    </tr>
  `).join('') || `<tr><td colspan="3"><div class="empty-state"><span class="empty-icon">○</span>No health checks reported</div></td></tr>`;
  $('#health-table').innerHTML = rows;

  $('#meta-table').innerHTML = `
    <tr><td>Service</td><td>event-platform-api</td></tr>
    <tr><td>Version</td><td>${escapeHtml(d.version || '—')}</td></tr>
    <tr><td>Runtime</td><td>${escapeHtml(d.node_version || 'native-c')}</td></tr>
    <tr><td>Uptime</td><td>${fmtUptime(d.uptime_seconds)}</td></tr>
    <tr><td>Last check</td><td>${fmtTime(d.timestamp || new Date().toISOString())}</td></tr>
  `;
}

function renderSystem(d) {
  // Topbar chips — show actual host info via colored pills
  const dev = d.device || {};
  const net = d.network || {};
  const chips = [];
  if (dev.brand || dev.model)
    chips.push(`<span class="chip region"><span class="chip-dot"></span>${escapeHtml(((dev.brand || '') + ' ' + (dev.model || '')).trim())}</span>`);
  if (d.platform) chips.push(`<span class="chip platform">${escapeHtml(d.platform)}</span>`);
  if (d.arch)     chips.push(`<span class="chip arch">${escapeHtml(d.arch)}</span>`);
  if (net.lan_ip) chips.push(`<span class="chip host">${escapeHtml(net.lan_ip)}:${net.port || 3001}</span>`);
  if (chips.length) $('#region-chips').innerHTML = chips.join('');

  $('#sys-cores').textContent = d.cpu?.cores ?? '—';
  $('#sys-cpu-model').textContent = d.cpu?.model && d.cpu.model !== 'unknown'
    ? d.cpu.model.slice(0, 32) : `${d.platform}/${d.arch}`;

  const l1 = d.cpu?.load_avg?.['1m'] ?? 0;
  $('#sys-load').textContent = l1.toFixed(2);
  $('#sys-load-detail').textContent =
    `5m ${(d.cpu?.load_avg?.['5m'] ?? 0).toFixed(2)} · 15m ${(d.cpu?.load_avg?.['15m'] ?? 0).toFixed(2)}`;
  /* Heuristic: load > cores = saturation */
  const cores = d.cpu?.cores || 8;
  applyStatus('sys-card-load', l1 > cores ? 'error' : l1 > cores * 0.7 ? 'warn' : 'ok');

  // Memory KPI + bar with threshold class
  const usedPct = d.memory?.used_percent ?? 0;
  $('#sys-mem-pct').textContent = usedPct.toFixed(1);
  $('#sys-mem-detail').textContent =
    `${fmtBytes(d.memory?.used_bytes)} of ${fmtBytes(d.memory?.total_bytes)}`;
  const memBar = $('#mem-bar');
  memBar.className = 'bar-stack ' + usageClass(usedPct);
  memBar.querySelector('.seg-used').style.width = `${usedPct}%`;
  memBar.querySelector('.seg-free').style.width = `${100 - usedPct}%`;
  applyStatus('sys-card-mem', usageClass(usedPct) === 'high' ? 'error' : usageClass(usedPct) === 'warn' ? 'warn' : 'ok');

  // Swap
  const swap = d.swap || {};
  const swapPct = swap.used_percent || 0;
  $('#sys-swap-pct').textContent = swapPct.toFixed(1);
  $('#sys-swap-detail').textContent = swap.total_bytes
    ? `${fmtBytes(swap.used_bytes)} of ${fmtBytes(swap.total_bytes)}`
    : 'no swap';
  const swapBar = $('#swap-bar');
  swapBar.className = 'bar-stack ' + usageClass(swapPct);
  swapBar.querySelector('.seg-used').style.width = `${swapPct}%`;
  swapBar.querySelector('.seg-free').style.width = `${100 - swapPct}%`;
  applyStatus('sys-card-swap', usageClass(swapPct) === 'high' ? 'error' : usageClass(swapPct) === 'warn' ? 'warn' : 'ok');

  // Disk
  const disk = d.disk || {};
  const diskPct = disk.used_percent || 0;
  $('#sys-disk-pct').textContent = diskPct;
  $('#sys-disk-detail').textContent = disk.total_bytes
    ? `${fmtBytes(disk.used_bytes)} of ${fmtBytes(disk.total_bytes)}`
    : 'unknown';
  const diskBar = $('#disk-bar');
  diskBar.className = 'bar-stack ' + usageClass(diskPct);
  diskBar.querySelector('.seg-used').style.width = `${diskPct}%`;
  diskBar.querySelector('.seg-free').style.width = `${100 - diskPct}%`;
  applyStatus('sys-card-disk', usageClass(diskPct) === 'high' ? 'error' : usageClass(diskPct) === 'warn' ? 'warn' : 'ok');

  const sysUp = d.uptime?.system_seconds || 0;
  const svcUp = d.uptime?.service_seconds || 0;
  $('#sys-uptime').textContent = fmtUptime(sysUp);
  $('#sys-uptime-detail').textContent = `service: ${fmtUptime(svcUp)} · process: ${fmtUptime(d.uptime?.process_seconds || 0)}`;

  $('#host-table').innerHTML = `
    <tr><td>Device</td><td>${escapeHtml(((dev.brand || '') + ' ' + (dev.model || '')).trim() || '—')}</td></tr>
    <tr><td>Android</td><td>${escapeHtml(dev.android_version || '?')}</td></tr>
    <tr><td>Architecture</td><td>${escapeHtml(d.platform)} / ${escapeHtml(d.arch)}</td></tr>
    <tr><td>CPU</td><td>${escapeHtml(d.cpu?.model || '—')} · ${d.cpu?.cores || 0} cores</td></tr>
    <tr><td>Hostname</td><td><code>${escapeHtml(d.hostname || '—')}</code></td></tr>
    <tr><td>LAN IP</td><td><code>${escapeHtml(net.lan_ip || '—')}:${net.port || 3001}</code></td></tr>
    <tr><td>System uptime</td><td>${fmtUptime(sysUp)}</td></tr>
    <tr><td>Service uptime</td><td>${fmtUptime(svcUp)}</td></tr>
    <tr><td>Process uptime</td><td>${fmtUptime(d.uptime?.process_seconds || 0)}</td></tr>
    <tr><td>Last reading</td><td class="muted small">${fmtTime(d.timestamp)}</td></tr>
  `;

  $('#mem-table').innerHTML = `
    <tr><td>Total</td><td>${fmtBytes(d.memory?.total_bytes)}</td></tr>
    <tr><td>Used</td><td>${fmtBytes(d.memory?.used_bytes)} (${usedPct.toFixed(1)}%)</td></tr>
    <tr><td>Available</td><td>${fmtBytes(d.memory?.available_bytes ?? d.memory?.free_bytes)}</td></tr>
    <tr><td>Free</td><td>${fmtBytes(d.memory?.free_bytes)}</td></tr>
    <tr><td>Buffers</td><td>${fmtBytes(d.memory?.buffers_bytes || 0)}</td></tr>
    <tr><td>Cached</td><td>${fmtBytes(d.memory?.cached_bytes || 0)}</td></tr>
  `;

  $('#swap-table').innerHTML = `
    <tr><td>Total</td><td>${fmtBytes(swap.total_bytes)}</td></tr>
    <tr><td>Used</td><td>${fmtBytes(swap.used_bytes)} (${swapPct.toFixed(1)}%)</td></tr>
    <tr><td>Free</td><td>${fmtBytes(swap.free_bytes)}</td></tr>
  `;

  $('#disk-table').innerHTML = `
    <tr><td>Total</td><td>${fmtBytes(disk.total_bytes)}</td></tr>
    <tr><td>Used</td><td>${fmtBytes(disk.used_bytes)} (${diskPct}%)</td></tr>
    <tr><td>Free</td><td>${fmtBytes(disk.free_bytes)}</td></tr>
  `;
}

function renderDb(d) {
  $('#db-version').textContent = d.database?.version || d.database_version || 'PostgreSQL';
  $('#db-size').textContent = fmtBytes(d.database?.size_bytes ?? d.database_size_bytes);
  const tables = d.tables || [];
  $('#db-tables-count').textContent = tables.length;

  $('#db-tables-body').innerHTML = tables.map(t => `
    <tr>
      <td><code>${escapeHtml(t.name)}</code></td>
      <td class="num">${(t.row_count ?? t.live_tuples ?? 0).toLocaleString()}</td>
      <td class="num">${fmtBytes(t.size_bytes ?? 0)}</td>
    </tr>
  `).join('') || `<tr><td colspan="3"><div class="empty-state"><span class="empty-icon">○</span>No user tables yet</div></td></tr>`;
}

function renderParticipantStats(d) {
  $('#kpi-participants').textContent = d.total ?? 0;
  $('#kpi-participants-sub').textContent =
    d.by_team?.length ? `${d.by_team.length} team${d.by_team.length > 1 ? 's' : ''}` : 'no teams';

  $('#part-total').textContent = d.total ?? 0;
  $('#part-teams-count').textContent = d.by_team?.length ?? 0;

  if (d.by_team?.length) {
    const top = d.by_team[0];
    $('#part-top-team').textContent = top.team;
    $('#part-top-team-sub').textContent = `${top.count} participant${top.count > 1 ? 's' : ''}`;
  } else {
    $('#part-top-team').textContent = '—';
    $('#part-top-team-sub').textContent = 'no data';
  }

  $('#team-body').innerHTML = (d.by_team || []).map(t => `
    <tr><td>${escapeHtml(t.team)}</td><td class="num">${t.count}</td></tr>
  `).join('') || `<tr><td colspan="2"><div class="empty-state"><span class="empty-icon">○</span>No teams yet</div></td></tr>`;

  $('#recent-body').innerHTML = (d.recent_5 || d.recent || []).map(p => `
    <tr>
      <td>${escapeHtml(p.name)}</td>
      <td>${escapeHtml(p.team)}</td>
      <td class="muted small">${fmtRel(p.createdAt)}</td>
    </tr>
  `).join('') || `<tr><td colspan="3"><div class="empty-state"><span class="empty-icon">○</span>No registrations yet</div></td></tr>`;
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
  `).join('') : `<tr><td colspan="6"><div class="empty-state"><span class="empty-icon">○</span>No participants registered yet</div></td></tr>`;

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

/* Render an error empty-state into a tbody when a section's fetch fails */
function renderError(tbodyId, cols, msg) {
  const el = document.getElementById(tbodyId);
  if (!el) return;
  el.innerHTML = `<tr><td colspan="${cols}"><div class="empty-state err"><span class="empty-icon">⚠</span>${escapeHtml(msg)}</div></td></tr>`;
}

function escapeHtml(s) {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[c]));
}

// ─── Main refresh ──────────────────────────────────────────────────────────
async function refresh() {
  const t0 = Date.now();
  setStatus('connecting', 'Refreshing…');
  $('#btn-refresh').classList.add('refreshing');

  /* Each section renders as soon as its data arrives. A slow /participants
   * must not delay /health rendering. */
  const sections = [
    {path: '/health/detailed',        render: renderHealth,           errorTarget: ['health-table', 3]},
    {path: '/system',                 render: renderSystem,           errorTarget: ['host-table', 2]},
    {path: '/stats/database',         render: renderDb,               errorTarget: ['db-tables-body', 3]},
    {path: '/stats/participants',     render: renderParticipantStats, errorTarget: ['team-body', 2]},
    {path: '/participants?limit=200', render: renderAllParticipants,  errorTarget: ['all-body', 6]},
  ];

  const results = await Promise.allSettled(
    sections.map(s => fetchJson(s.path).then(d => ({s, d})))
  );

  let okCount = 0, errCount = 0;
  for (let i = 0; i < results.length; i++) {
    const r = results[i];
    const s = sections[i];
    if (r.status === 'fulfilled') {
      try { s.render(r.value.d); okCount++; }
      catch (e) {
        console.error('render', s.path, e);
        errCount++;
        renderError(s.errorTarget[0], s.errorTarget[1], 'Render error: ' + e.message);
      }
    } else {
      console.error('fetch', s.path, r.reason);
      errCount++;
      renderError(s.errorTarget[0], s.errorTarget[1],
        'Failed to load (' + (r.reason?.message || 'network error') + ')');
    }
  }

  const elapsed = Date.now() - t0;
  $('#btn-refresh').classList.remove('refreshing');

  if (okCount === sections.length) {
    setStatus('ok', `Connected · ${elapsed}ms`);
    lastSuccessfulRefresh = new Date().toISOString();
    tickLastUpdate();
  } else if (okCount > 0) {
    setStatus('warn', `Partial · ${okCount}/${sections.length}`);
  } else {
    setStatus('error', 'Offline');
    $('#last-update').textContent = 'Failed to refresh';
  }
}

$('#btn-refresh').addEventListener('click', refresh);
$('#endpoint-text').textContent = new URL(API).host;

refresh();
/* Pause polling when tab is hidden, resume on focus. Saves cellular
 * data and avoids the rate-limit bump on browser-tab resume that
 * audit item 103 warned about. */
let refreshTimer = setInterval(refresh, REFRESH_MS);
document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    clearInterval(refreshTimer);
    refreshTimer = null;
  } else if (!refreshTimer) {
    refresh();
    refreshTimer = setInterval(refresh, REFRESH_MS);
  }
});
