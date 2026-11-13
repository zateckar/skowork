// Load test of the telemetry ingest (add-telemetry-and-admin task 4.3): simulated clients each send one OTLP batch a
// minute, spread evenly over the minute, to a control plane running in a child process on a file database. Reports
// the accept latency percentiles. Usage:
//   node scripts/load-telemetry.ts [--clients 30000] [--minutes 2] [--records 25] [--sockets 512]
import { spawn } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { Agent, request } from 'node:http';
import { randomUUID } from 'node:crypto';
import { exportJWK, generateKeyPair, SignJWT } from 'jose';
import { openDb } from '../src/db.ts';
import { encodeLogs, type LogBatch } from '../src/telemetry/otlp.ts';

const arg = (name: string, def: number) => {
  const i = process.argv.indexOf(`--${name}`);
  return i > 0 ? Number(process.argv[i + 1]) : def;
};
const CLIENTS = arg('clients', 30_000);
const MINUTES = arg('minutes', 2);
const RECORDS = arg('records', 25);
const SOCKETS = arg('sockets', 512);
const ISSUER = 'https://idp.load.test/realms/skowork';
const CLIENT_ID = 'skowork-desktop-load';

const dir = mkdtempSync(join(tmpdir(), 'skowork-load-'));
const dbPath = join(dir, 'cp.db');
const t0 = Date.now();
const log = (m: string) => console.log(`[${((Date.now() - t0) / 1000).toFixed(1)}s] ${m}`);

// ---- seed: one registered installation per client -------------------------------------------------------------------
const clients = Array.from({ length: CLIENTS }, (_, i) => ({ user: `load${String(i).padStart(6, '0')}`, installation: randomUUID(), token: '' }));
{
  const db = openDb(dbPath);
  const now = new Date().toISOString();
  const ins = db.prepare("INSERT INTO installations (id, subject, user_name, groups, version, first_seen, last_seen) VALUES (?, ?, ?, '[\"everyone\"]', '1.0.0', ?, ?)");
  db.exec('BEGIN');
  for (const c of clients) ins.run(c.installation, `sub-${c.user}`, c.user, now, now);
  db.exec('COMMIT');
  db.close();
}
log(`seeded ${CLIENTS} installations`);

const { privateKey, publicKey } = await generateKeyPair('ES256');
const jwks = { keys: [{ ...(await exportJWK(publicKey)), kid: 'load', alg: 'ES256', use: 'sig' }] };
const iat = Math.floor(Date.now() / 1000);
for (const c of clients) {
  c.token = await new SignJWT({ sub: `sub-${c.user}`, preferred_username: c.user, azp: CLIENT_ID, aud: [CLIENT_ID], sid: `sid-${c.user}`, roles: [] })
    .setProtectedHeader({ alg: 'ES256', kid: 'load', typ: 'JWT' })
    .setIssuer(ISSUER)
    .setIssuedAt(iat)
    .setExpirationTime(iat + 590) // under the 600 s lifetime limit; runs longer than about 8 minutes need fresh tokens
    .sign(privateKey);
}
log('signed tokens');

// ---- the control plane under test --------------------------------------------------------------------------------------
// --profile <dir> writes a CPU profile of the control plane there.
const profile = process.argv.includes('--profile') ? process.argv[process.argv.indexOf('--profile') + 1] : undefined;
// The profile is written on a normal exit, which a kill on Windows is not: the profiled process ends itself.
const profileArgs = profile
  ? ['--cpu-prof', `--cpu-prof-dir=${profile}`, '--import', `data:text/javascript,setTimeout(() => process.exit(0), ${MINUTES * 60_000 + 15_000})`]
  : [];
const server = spawn(process.execPath, [...profileArgs, 'src/main.ts'], {
  cwd: new URL('..', import.meta.url),
  env: {
    ...process.env,
    SKOWORK_ENV: 'test', SKOWORK_DB: dbPath, OIDC_CLIENT_ID: CLIENT_ID, SKOWORK_TEST_ISSUER: ISSUER, SKOWORK_TEST_ISSUER_JWKS: JSON.stringify(jwks),
    HOST: '127.0.0.1', PORT: '0', OPENAI_BASE_URL: '', OIDC_DISCOVERY_URL: '',
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
log(`control plane on port ${port}`);

// ---- load ------------------------------------------------------------------------------------------------------------
const EVENTS: [string, Record<string, string | number>][] = [
  ['tool.call', { 'run.id': 'r', 'tool.id': 'files.read', 'tool.risk': 'read', 'tool.outcome': 'ok', 'tool.duration_ms': 12 }],
  ['model.usage', { 'run.id': 'r', 'model.role': 'main', 'tokens.prompt': 1200, 'tokens.completion': 300 }],
  ['policy.decision', { 'run.id': 'r', 'tool.id': 'web.fetch', 'tool.risk': 'external', 'policy.rule': 'tools.risk', 'policy.version': 3, 'policy.outcome': 'approve' }],
  ['run.finished', { 'run.id': 'r', 'run.outcome': 'succeeded', 'run.origin': 'user', 'run.duration_ms': 8000, 'run.tool_calls': 4 }],
];
const body = (installation: string) => {
  const now = BigInt(Date.now()) * 1_000_000n;
  const batch: LogBatch = {
    resource: new Map<string, string>([['service.version', '1.0.0'], ['skowork.channel', 'stable'], ['os.build', '10.0.22631'], ['skowork.installation_id', installation]]),
    scopeName: 'skowork', scopeVersion: '1.0.0',
    records: Array.from({ length: RECORDS }, (_, i) => {
      const [event, attrs] = EVENTS[i % EVENTS.length]!;
      return { timeUnixNano: now, eventName: event, attributes: new Map(Object.entries({ ...attrs, 'run.id': randomUUID() })) };
    }),
  };
  return encodeLogs(batch);
};

const agent = new Agent({ keepAlive: true, maxSockets: SOCKETS });
const latencies: number[] = [];
const statuses = new Map<string, number>();
let firstError = '';
let inFlight = 0;
let maxInFlight = 0;
let retried = 0;
const send = (c: (typeof clients)[number]) =>
  new Promise<void>((resolve) => {
    const payload = body(c.installation);
    const start = performance.now();
    inFlight++;
    maxInFlight = Math.max(maxInFlight, inFlight);
    const attempt = (retry: boolean) => {
    const done = (status: string) => {
      inFlight--;
      latencies.push(performance.now() - start);
      statuses.set(status, (statuses.get(status) ?? 0) + 1);
      resolve();
    };
      const req = request(
        { host: '127.0.0.1', port, path: '/api/v1/telemetry/v1/logs', method: 'POST', agent,
          headers: { authorization: `Bearer ${c.token}`, 'content-type': 'application/x-protobuf', 'content-length': payload.length } },
        (res) => {
          let text = '';
          res.on('data', (d) => (text += d));
          res.on('end', () => {
            if (res.statusCode !== 200 && !firstError) firstError = `${res.statusCode} ${text}`;
            done(String(res.statusCode));
          });
        },
      );
      req.on('error', (e) => {
        // A kept-alive socket the server closed while idle: resent once, as an HTTP client does (latency includes it).
        if (retry && req.reusedSocket && (e as NodeJS.ErrnoException).code === 'ECONNRESET') {
          retried++;
          return attempt(false);
        }
        done(`error ${(e as NodeJS.ErrnoException).code ?? e.message}`);
      });
      req.end(payload);
    };
    attempt(true);
  });

// Each client sends at its own offset in every minute: N/60 batches a second, evenly spaced.
const total = CLIENTS * MINUTES;
const intervalMs = 60_000 / CLIENTS;
const startAt = performance.now();
const pending: Promise<void>[] = [];
let next = 0;
log(`sending ${total} batches of ${RECORDS} records at ${(CLIENTS / 60).toFixed(0)}/s for ${MINUTES} min`);
await new Promise<void>((resolve) => {
  const tick = () => {
    const due = Math.floor((performance.now() - startAt) / intervalMs);
    while (next <= due && next < total) pending.push(send(clients[next++ % CLIENTS]!));
    if (next >= total) return resolve();
    setTimeout(tick, 5);
  };
  tick();
});
const lag = performance.now() - startAt - total * intervalMs;
await Promise.all(pending);
const elapsed = (performance.now() - startAt) / 1000;

const sorted = latencies.slice().sort((a, b) => a - b);
const pct = (p: number) => sorted[Math.min(sorted.length - 1, Math.floor((p / 100) * sorted.length))]!.toFixed(1);
const accepted = statuses.get('200') ?? 0;
console.log(JSON.stringify({
  clients: CLIENTS, minutes: MINUTES, records_per_batch: RECORDS, batches: latencies.length, accepted,
  statuses: Object.fromEntries(statuses), elapsed_s: Number(elapsed.toFixed(1)), rate_per_s: Number((latencies.length / elapsed).toFixed(1)),
  latency_ms: { p50: pct(50), p95: pct(95), p99: pct(99), max: sorted.at(-1)!.toFixed(1) },
  max_in_flight: maxInFlight, resent_on_reused_socket: retried, generator_lag_ms: Math.round(Math.max(0, lag)), ...(firstError && { first_error: firstError.slice(0, 300) }),
}, null, 2));

if (!profile) server.kill('SIGTERM');
if (server.exitCode === null) await new Promise((r) => server.once('exit', r));
agent.destroy();
const size = ['cp.db', 'cp.db-wal'].reduce((s, f) => s + (existsSync(join(dir, f)) ? statSync(join(dir, f)).size : 0), 0);
console.log(`database: ${(size / 1e6).toFixed(0)} MB for ${accepted * RECORDS} records (${(size / Math.max(1, accepted * RECORDS)).toFixed(0)} bytes a record)`);
rmSync(dir, { recursive: true, force: true });
process.exit(accepted === latencies.length && Number(pct(99)) < 500 ? 0 : 1);
