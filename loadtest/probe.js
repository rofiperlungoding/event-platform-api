/* Probe what /participants actually returns + count after a bulk insert */
const API = process.argv[2] || 'https://api.rofidoesthings.site';
(async () => {
  const lr = await fetch(`${API}/auth/login`, {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({email: 'admin@intrivia.test', password: 'admin123'}),
  });
  const ld = await lr.json();
  const token = ld.token;

  const r = await fetch(`${API}/participants`, {headers: {'Authorization': `Bearer ${token}`}});
  console.log('status:', r.status);
  const txt = await r.text();
  console.log('len:', txt.length);
  console.log('first 400:', txt.slice(0, 400));
  try {
    const arr = JSON.parse(txt);
    console.log('count:', Array.isArray(arr) ? arr.length : 'not-array');
    if (Array.isArray(arr) && arr.length) {
      console.log('sample:', JSON.stringify(arr[0]));
      const stamp = arr.filter(p => p.email && p.email.startsWith('stamp-'));
      console.log('stamp matches:', stamp.length);
    }
  } catch (e) { console.log('parse err:', e.message); }
})();
