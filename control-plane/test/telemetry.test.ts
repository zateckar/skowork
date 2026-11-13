// control-plane/telemetry-ingest (add-telemetry-and-admin tasks 4.1, 4.2): authenticated OTLP log ingest, installation
// check, level enforcement, pseudonymized forwarding with an outage buffer, retention and rate limiting.
import { after, before, describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { createServer, type Server } from 'node:http';
import { register, startHarness, type Harness } from './helpers.ts';
import { decodeLogs, encodeLogs, type LogBatch, type Value } from '../src/telemetry/otlp.ts';
import { domainOf } from '../src/telemetry/ingest.ts';

function batch(installation: string, records: { event: string; attrs: Record<string, Value> }[], extraResource: Record<string, Value> = {}): Uint8Array {
  const b: LogBatch = {
    resource: new Map<string, Value>([['service.version', '0.1.0'], ['skowork.installation_id', installation], ...Object.entries(extraResource)]),
    scopeName: 'skowork.client',
    scopeVersion: '0.1.0',
    records: records.map((r, i) => ({ timeUnixNano: BigInt(Date.now() + i) * 1_000_000n, eventName: r.event, attributes: new Map(Object.entries(r.attrs)) })),
  };
  return encodeLogs(b);
}

async function post(h: Harness, token: string, body: Uint8Array, type = 'application/x-protobuf') {
  const r = await fetch(`${h.base}/api/v1/telemetry/v1/logs`, { method: 'POST', headers: { authorization: `Bearer ${token}`, 'content-type': type }, body: new Uint8Array(body) });
  const text = await r.text();
  let parsed: any = text;
  try {
    parsed = JSON.parse(text);
  } catch {
    /* text */
  }
  return { status: r.status, body: parsed, headers: r.headers };
}

const records = (h: Harness, user: string) =>
  (h.cp.db.prepare('SELECT event, attrs, installation_id FROM telemetry_records WHERE user_name = ? ORDER BY id').all(user) as { event: string; attrs: string; installation_id: string }[]).map(
    (r) => ({ ...r, attrs: JSON.parse(r.attrs) }),
  );

describe('telemetry ingest: authentication and levels', () => {
  let h: Harness;
  before(async () => {
    h = await startHarness();
  });
  after(() => h.close());

  it('round-trips the OTLP encoding', () => {
    const bytes = batch('i-1', [{ event: 'tool.call', attrs: { 'tool.id': 'files.read', 'tool.duration_ms': 12n, 'x.y': 1.5, ok: true } }]);
    const d = decodeLogs(bytes);
    assert.equal(d.resource.get('skowork.installation_id'), 'i-1');
    assert.equal(d.records[0]!.eventName, 'tool.call');
    assert.equal(d.records[0]!.attributes.get('tool.duration_ms'), 12n);
    assert.equal(d.records[0]!.attributes.get('x.y'), 1.5);
    assert.equal(d.records[0]!.attributes.get('ok'), true);
    assert.throws(() => decodeLogs(bytes.subarray(0, bytes.length - 2)));
  });

  it('accepts a valid batch from the user’s own installation and acknowledges it', async () => {
    const t = await h.token('tel1');
    const inst = (await register(h, t)).installation_id;
    const r = await post(h, t, batch(inst, [
      { event: 'run.finished', attrs: { 'run.id': 'r1', 'run.outcome': 'succeeded', 'run.duration_ms': 4200n } },
      { event: 'tool.call', attrs: { 'tool.id': 'web.fetch', 'tool.risk': 'read', 'url.domain': 'https://intranet.corp/hr/salaries?q=1' } },
    ]));
    assert.equal(r.status, 200, JSON.stringify(r.body));
    assert.equal(r.body.accepted, 2);
    assert.equal(r.body.level, 'metadata');
    const rows = records(h, 'tel1');
    assert.equal(rows.length, 2);
    assert.equal(rows[0]!.attrs['run.duration_ms'], 4200);
    assert.equal(rows[1]!.attrs['url.domain'], 'intranet.corp'); // re-reduced at ingest
    assert.equal(rows[1]!.installation_id, inst);
  });

  it('refuses a batch naming another user’s installation and audits it', async () => {
    const owner = await h.token('tel2');
    const inst = (await register(h, owner)).installation_id;
    const spoofer = await h.token('tel3');
    const r = await post(h, spoofer, batch(inst, [{ event: 'client.started', attrs: { 'process.startup_ms': 900n } }]));
    assert.equal(r.status, 403);
    assert.equal(r.body.error.code, 'INSTALLATION_MISMATCH');
    assert.equal(records(h, 'tel3').length, 0);
    const audit = h.cp.audit.query({ action: 'telemetry.installation-mismatch' });
    assert.ok(audit.some((a: any) => a.user_name === 'tel3' && a.installation_id === inst && a.outcome === 'refused'));
    const unknown = await post(h, spoofer, batch('not-registered', [{ event: 'client.started', attrs: {} }]));
    assert.equal(unknown.status, 403);
  });

  it('drops content fields and unknown fields from a metadata-level sender and counts the violation', async () => {
    const t = await h.token('tel4');
    const inst = (await register(h, t)).installation_id;
    const before = { ...h.cp.telemetry.stats };
    const r = await post(h, t, batch(inst, [
      { event: 'run.finished', attrs: { 'run.id': 'r2', 'content.prompt': 'please summarise the salary list', 'user.email': 'x@y' } },
      { event: 'not.an.event', attrs: { 'tool.id': 'x' } },
    ], { 'host.name': 'PC-SECRET' }));
    assert.equal(r.status, 200);
    assert.equal(r.body.accepted, 1);
    const rows = records(h, 'tel4');
    assert.deepEqual(Object.keys(rows[0]!.attrs), ['run.id']);
    assert.equal(h.cp.telemetry.stats.content_violations - before.content_violations, 1);
    assert.equal(h.cp.telemetry.stats.dropped_unknown - before.dropped_unknown, 3); // user.email, the event, host.name
  });

  it('keeps content fields when the sender’s policy sets the content level', async () => {
    const admin = await h.token('admin1');
    await h.call('POST', '/api/v1/admin/identity/group-members', admin, { user_name: 'tel5', skowork_group: 'developers' });
    const put = await h.call('PUT', '/api/v1/admin/policy/groups/developers/draft', admin, { document: { telemetry: { level: 'content' } }, priority: 10, reason: 'test' });
    assert.equal(put.status, 200, JSON.stringify(put.body));
    assert.equal((await h.call('POST', '/api/v1/admin/policy/groups/developers/publish', admin, { reason: 'test' })).status, 200);
    const t = await h.token('tel5');
    const inst = (await register(h, t)).installation_id;
    const r = await post(h, t, batch(inst, [{ event: 'run.finished', attrs: { 'run.id': 'r3', 'content.prompt': 'hello' } }]));
    assert.equal(r.body.level, 'content');
    assert.equal(records(h, 'tel5')[0]!.attrs['content.prompt'], 'hello');
  });

  it('rejects malformed and non-protobuf bodies', async () => {
    const t = await h.token('tel6');
    const inst = (await register(h, t)).installation_id;
    assert.equal((await post(h, t, new TextEncoder().encode('{}'), 'application/json')).status, 415);
    assert.equal((await post(h, t, Uint8Array.from([0x0a, 0xff, 0xff]))).status, 400);
    assert.equal((await post(h, t, batch('', []))).status, 400);
    assert.equal((await post(h, t, batch(inst, []))).status, 200);
    const anon = await fetch(`${h.base}/api/v1/telemetry/v1/logs`, { method: 'POST', headers: { 'content-type': 'application/x-protobuf' }, body: new Uint8Array(batch(inst, [])) });
    assert.equal(anon.status, 401);
  });

  it('reduces domains defensively', () => {
    assert.equal(domainOf('https://A.Example.com:8443/x?y'), 'a.example.com');
    assert.equal(domainOf('user:pw@host.corp:80/p'), 'host.corp');
    assert.equal(domainOf('has spaces'), '');
  });
});

describe('telemetry ingest: forwarding, retention, rate limits', () => {
  let h: Harness;
  let target: Server;
  let targetUrl = '';
  let up = false;
  const received: Uint8Array[] = [];
  before(async () => {
    target = createServer((req, res) => {
      const chunks: Buffer[] = [];
      req.on('data', (c: Buffer) => chunks.push(c));
      req.on('end', () => {
        if (!up) {
          res.writeHead(503).end();
          return;
        }
        received.push(new Uint8Array(Buffer.concat(chunks)));
        res.writeHead(200, { 'content-type': 'application/json' }).end('{}');
      });
    });
    await new Promise<void>((r) => target.listen(0, '127.0.0.1', () => r()));
    const addr = target.address();
    targetUrl = `http://127.0.0.1:${typeof addr === 'object' && addr ? addr.port : 0}/v1/logs`;
    h = await startHarness((c) => {
      c.telemetry.forwardUrl = targetUrl;
      c.telemetry.pseudonymize = true;
      c.telemetry.forwardIntervalMs = 3_600_000; // driven by the test
      c.telemetry.batchesPerMinute = 6;
      c.telemetry.batchBurst = 3;
    });
  });
  after(async () => {
    await h.close();
    await new Promise<void>((r) => target.close(() => r()));
  });

  it('keeps accepting while the target is down and forwards the buffer in order once it recovers, pseudonymized', async () => {
    const t = await h.token('fwd1');
    const inst = (await register(h, t)).installation_id;
    up = false;
    for (const id of ['a', 'b']) {
      const r = await post(h, t, batch(inst, [{ event: 'run.finished', attrs: { 'run.id': id } }]));
      assert.equal(r.status, 200);
    }
    await h.cp.telemetry.forwardOnce();
    assert.equal(received.length, 0);
    assert.equal(h.cp.telemetry.outboxSize(), 2);
    assert.ok(h.cp.telemetry.stats.forward_failures >= 1);

    up = true;
    await h.cp.telemetry.forwardOnce();
    assert.equal(received.length, 2);
    assert.equal(h.cp.telemetry.outboxSize(), 0);
    const first = decodeLogs(received[0]!);
    assert.equal(first.records[0]!.attributes.get('run.id'), 'a');
    assert.equal(decodeLogs(received[1]!).records[0]!.attributes.get('run.id'), 'b');
    const enduser = String(first.resource.get('enduser.id'));
    assert.match(enduser, /^u_[0-9a-f]{20}$/);
    assert.notEqual(enduser, 'fwd1');
    assert.match(String(first.resource.get('host.name')), /^m_[0-9a-f]{20}$/);
    // Stable, and re-identifiable only through the stored mapping.
    assert.equal(h.cp.telemetry.pseudonym('user', 'fwd1'), enduser);
    assert.deepEqual(h.cp.telemetry.reidentify(enduser), { kind: 'user', value: 'fwd1' });
  });

  it('deletes stored records after the retention period and expires an old forwarding buffer', async () => {
    const t = await h.token('ret1');
    const inst = (await register(h, t)).installation_id;
    up = false;
    await post(h, t, batch(inst, [{ event: 'client.started', attrs: { 'process.startup_ms': 800n } }]));
    assert.equal(records(h, 'ret1').length, 1);
    const day = 86_400_000;
    h.cp.telemetry.prune(Date.now() + 29 * day);
    assert.equal(records(h, 'ret1').length, 1);
    assert.equal(h.cp.telemetry.outboxSize(), 0, 'the 48-hour forwarding buffer expired after 29 days');
    assert.ok(h.cp.telemetry.stats.forward_expired >= 1);
    h.cp.telemetry.prune(Date.now() + 31 * day);
    assert.equal(records(h, 'ret1').length, 0);
  });

  it('answers a flooding installation with a retry-after indication', async () => {
    const t = await h.token('flood1');
    const inst = (await register(h, t)).installation_id;
    const statuses: number[] = [];
    let retry = '';
    for (let i = 0; i < 5; i++) {
      const r = await post(h, t, batch(inst, []));
      statuses.push(r.status);
      if (r.status === 429) {
        retry = r.headers.get('retry-after') ?? '';
        assert.equal(r.body.error.code, 'RATE_LIMITED');
        assert.ok(r.body.error.retry_after_s >= 1);
      }
    }
    assert.deepEqual(statuses.slice(0, 3), [200, 200, 200]);
    assert.equal(statuses[3], 429);
    assert.ok(Number(retry) >= 1);
    // Another installation is not affected.
    const other = (await register(h, t)).installation_id;
    assert.equal((await post(h, t, batch(other, []))).status, 200);
    const stats = await h.call('GET', '/api/v1/admin/telemetry/stats', await h.token('admin1'));
    assert.equal(stats.status, 200);
    assert.ok(stats.body.rate_limited >= 1);
    assert.equal((await h.call('GET', '/api/v1/admin/telemetry/stats', t)).status, 403);
  });
});


describe('telemetry ingest: crash reports', () => {
  let h: Harness;
  before(async () => {
    h = await startHarness();
  });
  after(() => h.close());

  const upload = async (token: string, headers: Record<string, string>, body: Uint8Array) => {
    const r = await fetch(`${h.base}/api/v1/telemetry/v1/crashes`, {
      method: 'POST',
      headers: { authorization: `Bearer ${token}`, 'content-type': 'application/octet-stream', ...headers },
      body: new Uint8Array(body),
    });
    return { status: r.status, body: await r.json().catch(() => null) };
  };
  const meta = (id: string, inst: string) => ({
    'x-skowork-crash-id': id, 'x-skowork-crash-process': 'worker-extract', 'x-skowork-crash-exception_code': '0xC000001D',
    'x-skowork-crash-module': 'skowork-worker.exe', 'x-skowork-crash-installation_id': inst,
  });

  it('stores a dump from the user’s own installation encrypted, and decrypts it', async () => {
    const t = await h.token('crash1');
    const inst = (await register(h, t)).installation_id;
    const dump = new TextEncoder().encode('MDMP' + 'stack of a worker thread '.repeat(50));
    const r = await upload(t, meta('crash-aaaaaaaa', inst), dump);
    assert.equal(r.status, 202, JSON.stringify(r.body));
    const row = h.cp.db.prepare('SELECT user_name, installation_id, process, size, data FROM telemetry_crashes WHERE id = ?').get('crash-aaaaaaaa') as any;
    assert.equal(row.user_name, 'crash1');
    assert.equal(row.process, 'worker-extract');
    assert.equal(row.size, dump.length);
    assert.equal(Buffer.from(row.data).indexOf('stack of a worker'), -1); // not stored in the clear
    assert.deepEqual(new Uint8Array(h.cp.telemetry.readCrash('crash-aaaaaaaa')!), dump);
  });

  it('refuses another user’s installation, a missing id and an empty body', async () => {
    const owner = await h.token('crash2');
    const inst = (await register(h, owner)).installation_id;
    const other = await h.token('crash3');
    const dump = new TextEncoder().encode('MDMP');
    const spoofed = await upload(other, meta('crash-bbbbbbbb', inst), dump);
    assert.equal(spoofed.status, 403);
    assert.equal(spoofed.body.error.code, 'INSTALLATION_MISMATCH');
    assert.equal((await upload(owner, meta('x', inst), dump)).status, 400);
    assert.equal((await upload(owner, meta('crash-cccccccc', inst), new Uint8Array())).status, 400);
    assert.equal(h.cp.db.prepare('SELECT COUNT(*) AS n FROM telemetry_crashes WHERE user_name IN (?, ?)').get('crash2', 'crash3')!.n, 0);
  });

  it('refuses crash reports while the user’s telemetry level is off', async () => {
    const admin = await h.token('admin1');
    await h.call('POST', '/api/v1/admin/identity/group-members', admin, { user_name: 'crash4', skowork_group: 'quiet' });
    assert.equal((await h.call('PUT', '/api/v1/admin/policy/groups/quiet/draft', admin, { document: { telemetry: { level: 'off' } }, priority: 10, reason: 'test' })).status, 200);
    assert.equal((await h.call('POST', '/api/v1/admin/policy/groups/quiet/publish', admin, { reason: 'test' })).status, 200);
    const t = await h.token('crash4');
    const inst = (await register(h, t)).installation_id;
    const r = await upload(t, meta('crash-dddddddd', inst), new TextEncoder().encode('MDMP'));
    assert.equal(r.status, 403);
    assert.equal(r.body.error.code, 'TELEMETRY_OFF');
  });
});