#!/usr/bin/env node
/**
 * Helper: log in as admin, create an event + active session, print session code.
 * Stdout: just the session code on the last line.
 *
 * Usage: node setup-session.js [api_url]
 */
const API = process.argv[2] || 'http://localhost:3001';
const EMAIL = process.env.ADMIN_EMAIL || 'admin@intrivia.test';
const PASSWORD = process.env.ADMIN_PASSWORD || 'admin123';

(async () => {
  try {
    const lr = await fetch(`${API}/auth/login`, {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({email: EMAIL, password: PASSWORD}),
    });
    const ld = await lr.json();
    if (!lr.ok) throw new Error('login: ' + (ld.error || lr.status));
    const token = ld.token;
    console.error('admin token acquired');

    /* Pick first event or create one */
    const er = await fetch(`${API}/events`, {headers: {'Authorization': `Bearer ${token}`}});
    let events = [];
    try { events = await er.json(); if (!Array.isArray(events)) events = []; } catch (e) {}
    let eventId;
    if (events.length > 0) {
      eventId = events[0].id;
      console.error('reuse event id', eventId);
    } else {
      const ce = await fetch(`${API}/events`, {
        method: 'POST',
        headers: {'Content-Type': 'application/json', 'Authorization': `Bearer ${token}`},
        body: JSON.stringify({name: 'Stampede Test', description: 'auto', starts_at: new Date().toISOString()}),
      });
      const ed = await ce.json();
      if (!ce.ok) throw new Error('create event: ' + (ed.error || ce.status));
      eventId = ed.id;
      console.error('created event id', eventId);
    }

    /* Create session valid for 2h via /sessions/create (default expiry) */
    const cs = await fetch(`${API}/sessions/create`, {
      method: 'POST',
      headers: {'Content-Type': 'application/json', 'Authorization': `Bearer ${token}`},
      body: JSON.stringify({}),
    });
    const sd = await cs.json();
    if (!cs.ok) throw new Error('create session: ' + (sd.error || cs.status));
    console.error('session created');
    /* Print only the code on stdout */
    process.stdout.write(sd.code + '\n');
  } catch (e) {
    console.error('FAIL:', e.message);
    process.exit(1);
  }
})();
