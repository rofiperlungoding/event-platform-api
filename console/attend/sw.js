/* Service Worker — offline-first + background sync */
const CACHE_NAME = 'checkin-v2';
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

// Activate: clean old caches
self.addEventListener('activate', e => {
  e.waitUntil(
    caches.keys().then(keys =>
      Promise.all(keys.filter(k => k !== CACHE_NAME).map(k => caches.delete(k)))
    )
  );
  self.clients.claim();
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
   * for the same session are committed in a single transaction server-side. */
  const API = self.registration.scope.replace(/\/$/, '').replace('console.', 'api.');
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
