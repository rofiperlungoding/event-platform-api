/* Service Worker — offline-first + background sync.
 *
 * Round 7 hardening: bumped CACHE_NAME so the new install/activate
 * runs and the page-side `sw-updated` banner fires for every
 * attendee on next reload. The CDN-hosted html5-qrcode script is
 * pinned to an exact version (audit item 223); supply-chain risk
 * exists but is mitigated by the version pin and SRI is a future
 * polish. */
const CACHE_NAME = 'checkin-v5';
const STATIC_ASSETS = [
  '/attend/scan.html',
  '/attend/manifest.json',
  '/attend/icon-192.svg',
  'https://unpkg.com/html5-qrcode@2.3.8/html5-qrcode.min.js',
];

// Install: cache static assets
self.addEventListener('install', e => {
  e.waitUntil(
    caches.open(CACHE_NAME).then(c => c.addAll(STATIC_ASSETS))
  );
  self.skipWaiting();
});

// Activate: clean old caches and tell clients we have a new version
self.addEventListener('activate', e => {
  e.waitUntil((async () => {
    const keys = await caches.keys();
    await Promise.all(keys.filter(k => k !== CACHE_NAME).map(k => caches.delete(k)));
    await self.clients.claim();
    /* Notify all clients so the page can show a "refresh available"
     * banner without forcing a reload. Mitigates audit item 121. */
    const clients = await self.clients.matchAll({ type: 'window' });
    for (const client of clients) {
      client.postMessage({ type: 'sw-updated', cacheName: CACHE_NAME });
    }
  })());
});

// Fetch: serve from cache first, fallback to network
self.addEventListener('fetch', e => {
  const url = new URL(e.request.url);

  // API calls: network-first (don't cache)
  if (url.pathname.startsWith('/auth/') ||
      url.pathname.startsWith('/attendance/') ||
      url.pathname.startsWith('/device/') ||
      url.pathname.startsWith('/sessions/') ||
      url.pathname.startsWith('/health')) {
    return; // let browser handle normally
  }

  // Static assets: cache-first
  e.respondWith(
    caches.match(e.request).then(cached => {
      if (cached) return cached;
      return fetch(e.request).then(resp => {
        if (resp.ok) {
          const clone = resp.clone();
          caches.open(CACHE_NAME).then(c => c.put(e.request, clone));
        }
        return resp;
      });
    }).catch(() => caches.match('/attend/scan.html'))
  );
});

// Background sync: process queued check-ins
self.addEventListener('sync', e => {
  if (e.tag === 'sync-checkins') {
    e.waitUntil(syncCheckins());
  }
});

async function syncCheckins() {
  /* Drain the offline queue using /attendance/batch-checkin so all items
   * for the same session are committed in a single transaction server-side.
   *
   * Round 7 fix (audit item 221): the previous URL derivation
   * (`scope.replace('console.', 'api.')`) only worked when the PWA
   * was hosted under a `console.` subdomain. For a tablet that
   * runs the API and PWA on the same origin (LAN deployments), the
   * derived URL would be wrong and every batch would fail. We now
   * try the same origin first, then fall back to a list of known
   * candidates. The candidate list mirrors what `resolveApi` in
   * scan.html accepts. */
  const candidates = [];
  candidates.push(self.registration.scope.replace(/\/$/, ''));
  candidates.push(self.registration.scope.replace(/\/$/, '').replace('console.', 'api.'));
  candidates.push('https://api.rofidoesthings.site');
  candidates.push('http://192.168.100.67:3001');
  /* De-dupe while preserving order */
  const seen = new Set();
  const tryOrder = candidates.filter(u => !seen.has(u) && seen.add(u));

  let API = null;
  for (const u of tryOrder) {
    try {
      const r = await fetch(`${u}/health`, { cache: 'no-store' });
      if (r.ok) { API = u; break; }
    } catch (_) {}
  }
  if (!API) return;     /* nothing reachable; retry on next sync event */

  const db = await openDB();
  const items = await getAllFromStore(db.transaction('queue', 'readonly').objectStore('queue'));
  if (items.length === 0) return;

  const bySession = {};
  for (const it of items) {
    const code = it.code || (it.body && JSON.parse(it.body).session_code);
    const device_uuid = it.device_uuid;
    if (!code || !device_uuid) continue;
    (bySession[code] = bySession[code] || []).push({id: it.id, device_uuid});
  }

  for (const [code, group] of Object.entries(bySession)) {
    try {
      const r = await fetch(`${API}/attendance/batch-checkin`, {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
          session_code: code,
          items: group.map(g => ({device_uuid: g.device_uuid})),
        }),
      });
      if (!r.ok) continue;
      const dtx = db.transaction('queue', 'readwrite');
      const dst = dtx.objectStore('queue');
      for (const g of group) dst.delete(g.id);
    } catch (e) { break; }
  }
}

function openDB() {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open('checkin-queue', 1);
    req.onupgradeneeded = e => {
      e.target.result.createObjectStore('queue', {keyPath: 'id', autoIncrement: true});
    };
    req.onsuccess = e => resolve(e.target.result);
    req.onerror = e => reject(e.target.error);
  });
}

function getAllFromStore(store) {
  return new Promise((resolve, reject) => {
    const req = store.getAll();
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}
