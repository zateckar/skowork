// Load test of the fleet registry's push channel (add-client-foundation task 5.5): simulated clients each hold one
// push WebSocket to a control plane running in a child process on a file database, then an administrator sends
// pushes to a Group that contains all of them. Reports the handshake rate and, per push, the latency from the admin
// request to each client's receipt (the clients acknowledge every message, as the desktop client does). Usage:
//   node scripts/load-push.ts [--clients 30000] [--pushes 5] [--sources 8] [--handshakes 256]
// The clients connect from 127.0.0.2..127.0.0.<sources + 1> on explicit ports below the dynamic port range.
import { execFileSync, spawn } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { randomUUID } from 'node:crypto';
import { exportJWK, generateKeyPair, SignJWT } from 'jose';
import WebSocket from 'ws';
import { openDb } from '../src/db.ts';

const arg = (name: string, def: number) => {
  const i = process.argv.indexOf(`--${name}`);
  return i > 0 ? Number(process.argv[i + 1]) : def;
};
const CLIENTS = arg('clients', 30_000);
const PUSHES = arg('pushes', 5);
const SOURCES = arg('sources', 8);
const HANDSHAKES = arg('handshakes', 256);
// Local ports below the dynamic range, from a random base so that a rerun does not meet the last run's TIME_WAIT.
const PORT_BASE = 10_000 + Math.floor(Math.random() * 6) * 1000;
const ISSUER = 'https://idp.load.test/realms/workbench';
const CLIENT_ID = 'workbench-desktop-load';
const GROUP = 'load-all';

const dir = mkdtempSync(join(tmpdir(), 'workbench-load-'));
const dbPath = join(dir, 'cp.db');
const t0 = Date.now();
const log = (m: string) => console.log(`[${((Date.now() - t0) / 1000).toFixed(1)}s] ${m}`);

// ---- seed: one registered installation per client, all in one Group ---------------------------------------------------
const clients = Array.from({ length: CLIENTS }, (_, i) => ({ user: `load${String(i).padStart(6, '0')}`, installation: randomUUID(), token: '' }));
{
  const db = openDb(dbPath);
  const now = new Date().toISOString();
  const ins = db.prepare(`INSERT INTO installations (id, subject, user_name, groups, version, first_seen, last_seen) VALUES (?, ?, ?, '["everyone","${GROUP}"]', '1.0.0', ?, ?)`);
  db.exec('BEGIN');
  for (const c of clients) ins.run(c.installation, `sub-${c.user}`, c.user, now, now);
  db.exec('COMMIT');
  db.close();
}
log(`seeded ${CLIENTS} installations`);

const { privateKey, publicKey } = await generateKeyPair('ES256');
const jwks = { keys: [{ ...(await exportJWK(publicKey)), kid: 'load', alg: 'ES256', use: 'sig' }] };
const iat = Math.floor(Date.now() / 1000);
const sign = (user: string) =>
  new SignJWT({ sub: `sub-${user}`, preferred_username: user, azp: CLIENT_ID, aud: [CLIENT_ID], sid: `sid-${user}`, roles: [] })
    .setProtectedHeader({ alg: 'ES256', kid: 'load', typ: 'JWT' })
    .setIssuer(ISSUER)
    .setIssuedAt(iat)
    .setExpirationTime(iat + 590) // under the 600 s lifetime limit; the run must finish before the tokens expire
    .sign(privateKey);
for (const c of clients) c.token = await sign(c.user);
const adminToken = await sign('admin1');
log('signed tokens');

// ---- the control plane under test --------------------------------------------------------------------------------------
const server = spawn(process.execPath, ['src/main.ts'], {
  cwd: new URL('..', import.meta.url),
  env: {
    ...process.env,
    WORKBENCH_ENV: 'test', WORKBENCH_DB: dbPath, OIDC_CLIENT_ID: CLIENT_ID, WORKBENCH_TEST_ISSUER: ISSUER, WORKBENCH_TEST_ISSUER_JWKS: JSON.stringify(jwks),
    WORKBENCH_BOOTSTRAP_ADMINS: 'admin1', HOST: '127.0.0.1', PORT: '0', OPENAI_BASE_URL: '', OIDC_DISCOVERY_URL: '',
  },
  stdio: ['ignore', 'pipe', 'inherit'],
});
const port = await new Promise<number>((resolve, reject) => {
  let out = '';
  server.stdout!.on('data', (d) => {
    out += d;
    const m = /listening on [^:]+:(\d+)/.exec(out);
    if (m) resolve(Number(m[1]));
  });
  server.once('exit', (code) => reject(new Error(`the control plane exited with ${code}`)));
});
log(`control plane on port ${port} (pid ${server.pid})`);
const serverMb = () => {
  try {
    const out = execFileSync('powershell.exe', ['-NoProfile', '-Command', `(Get-Process -Id ${server.pid}).WorkingSet64`], { encoding: 'utf8' });
    return Math.round(Number(out.trim()) / 1e6);
  } catch {
    return -1;
  }
};

// ---- connect -----------------------------------------------------------------------------------------------------------
// received[push][client] = ms since the push request was sent
let pushStart = 0;
let round = -1;
const received: number[][] = Array.from({ length: PUSHES }, () => []);
const sockets: WebSocket[] = [];
const handshakeErrors = new Map<string, number>();
let closedEarly = 0;
let retries = 0;
const connect = (i: number, attempt = 0): Promise<void> =>
  new Promise<void>((resolve) => {
    const c = clients[i]!;
    let open = false;
    const ws = new WebSocket(`ws://127.0.0.1:${port}/api/v1/fleet/push?installation_id=${c.installation}&last_ack=0`, {
      headers: { authorization: `Bearer ${c.token}` },
      // An explicit port: Windows allocates ephemeral ports across all local addresses, about 16k in total.
      localAddress: `127.0.0.${2 + (i % SOURCES)}`,
      localPort: PORT_BASE + attempt * 4000 + Math.floor(i / SOURCES),
      perMessageDeflate: false,
    });
    ws.once('open', () => ((open = true), sockets.push(ws), resolve()));
    ws.once('unexpected-response', (_req, res) => {
      handshakeErrors.set(String(res.statusCode), (handshakeErrors.get(String(res.statusCode)) ?? 0) + 1);
      resolve();
    });
    ws.once('error', (e) => {
      const code = (e as NodeJS.ErrnoException).code ?? e.message;
      // A full listen backlog refuses the connect; the desktop client reconnects with backoff, so retry likewise.
      if (!open && ['ECONNREFUSED', 'ECONNRESET', 'EADDRINUSE', 'EACCES'].includes(code) && attempt < 5) {
        retries++;
        setTimeout(() => void connect(i, attempt + 1).then(resolve), 100 * 2 ** attempt);
        return;
      }
      handshakeErrors.set(code, (handshakeErrors.get(code) ?? 0) + 1);
      resolve();
    });
    ws.on('close', () => open && closedEarly++);
    ws.on('message', (d) => {
      const m = JSON.parse(d.toString());
      if (typeof m.id !== 'number') return;
      if (round >= 0 && m.payload?.round === round) received[round]!.push(performance.now() - pushStart);
      ws.send(JSON.stringify({ type: 'ack', ref: m.id }));
    });
  });

const hs0 = performance.now();
{
  let next = 0;
  const worker = async () => {
    while (next < CLIENTS) await connect(next++);
  };
  await Promise.all(Array.from({ length: HANDSHAKES }, worker));
}
const hsSecs = (performance.now() - hs0) / 1000;
log(`connected ${sockets.length} of ${CLIENTS} in ${hsSecs.toFixed(1)} s (${(sockets.length / hsSecs).toFixed(0)}/s); ${retries} connects retried; errors ${JSON.stringify(Object.fromEntries(handshakeErrors))}`);
const idleMb = serverMb();
log(`control plane working set with ${sockets.length} idle connections: ${idleMb} MB`);

// ---- pushes ------------------------------------------------------------------------------------------------------------
const perPush: { recipients: number; received: number; request_ms: number; p50: number; p99: number; max: number }[] = [];
const all: number[] = [];
const pct = (s: number[], p: number) => (s.length ? s[Math.min(s.length - 1, Math.floor((p / 100) * s.length))]! : NaN);
for (let p = 0; p < PUSHES; p++) {
  await new Promise((r) => setTimeout(r, 2000));
  round = p;
  pushStart = performance.now();
  const res = await fetch(`http://127.0.0.1:${port}/api/v1/admin/fleet/push`, {
    method: 'POST',
    headers: { authorization: `Bearer ${adminToken}`, 'content-type': 'application/json' },
    body: JSON.stringify({ type: 'policy-changed', target: { group: GROUP }, payload: { round: p, version: `load-${p}` } }),
  });
  const body = (await res.json()) as { recipients?: number };
  const requestMs = performance.now() - pushStart;
  const deadline = performance.now() + 30_000;
  while (received[p]!.length < sockets.length && performance.now() < deadline) await new Promise((r) => setTimeout(r, 20));
  const s = received[p]!.slice().sort((a, b) => a - b);
  all.push(...s);
  perPush.push({ recipients: body.recipients ?? -1, received: s.length, request_ms: Math.round(requestMs), p50: Math.round(pct(s, 50)), p99: Math.round(pct(s, 99)), max: Math.round(s.at(-1) ?? NaN) });
  log(`push ${p}: ${JSON.stringify(perPush.at(-1))}`);
}
all.sort((a, b) => a - b);
const missing = perPush.reduce((n, x) => n + (sockets.length - x.received), 0);
const peakMb = serverMb();

console.log(JSON.stringify({
  clients: CLIENTS, connected: sockets.length, handshake_rate_per_s: Math.round(sockets.length / hsSecs), connects_retried: retries, pushes: PUSHES,
  latency_ms: { p50: Math.round(pct(all, 50)), p95: Math.round(pct(all, 95)), p99: Math.round(pct(all, 99)), max: Math.round(all.at(-1) ?? NaN) },
  missing_deliveries: missing, closed_during_run: closedEarly, server_working_set_mb: { idle_connected: idleMb, after_pushes: peakMb },
}, null, 2));

for (const ws of sockets) ws.terminate();
server.kill('SIGTERM');
if (server.exitCode === null) await new Promise((r) => server.once('exit', r));
const size = ['cp.db', 'cp.db-wal'].reduce((s, f) => s + (existsSync(join(dir, f)) ? statSync(join(dir, f)).size : 0), 0);
console.log(`database: ${(size / 1e6).toFixed(0)} MB`);
rmSync(dir, { recursive: true, force: true });
process.exit(sockets.length === CLIENTS && missing === 0 && pct(all, 99) < 5000 ? 0 : 1);
