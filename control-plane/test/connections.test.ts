// LLM connections (add-llm-connections tasks 1.1-1.3): storage and encryption, bootstrap, discovery, routing to
// connections and the admin API, against in-test OpenAI-compatible upstreams.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { loadConfig, type Config } from '../src/config.ts';
import { openDb } from '../src/db.ts';
import { Audit } from '../src/identity/audit.ts';
import { Connections, serverKey, StartupError } from '../src/gateway/connections.ts';
import { fakeUpstream, json, startHarness, type FakeUpstream, type Harness } from './helpers.ts';

const KEY_A = 'upstream-key-a-0123456789';
const KEY_B = 'upstream-key-b-9876543210';

const listing = (f: FakeUpstream, ids: string[], status = 200) => {
  const base = f.handler;
  f.handler = (path, body, res) => (path.endsWith('/models') ? json(res, status, status === 200 ? { data: ids.map((id) => ({ id, object: 'model' })) } : { error: { message: 'bad key sk-SECRET' } }) : base(path, body, res));
};

async function admin(h: Harness) {
  const t = await h.token('admin1');
  return {
    t,
    call: (method: string, path: string, body?: unknown) => h.call(method, `/api/v1/admin/llm${path}`, t, body),
  };
}

const chat = (h: Harness, t: string) => h.call('POST', '/llm/v1/chat/completions', t, { model: 'main', messages: [{ role: 'user', content: 'hi' }] });
const sent = (f: FakeUpstream) => f.requests.filter((r) => !r.path.endsWith('/models'));

test('connections: bootstrap from the environment only when empty; credentials encrypted and write-only', async () => {
  const dir = mkdtempSync(join(tmpdir(), 'skowork-conn-'));
  const a = await fakeUpstream();
  const b = await fakeUpstream();
  try {
    let h = await startHarness((c) => {
      c.dbPath = join(dir, 'cp.db');
      c.gateway.upstreams = [{ name: 'primary', baseUrl: a.url, apiKey: KEY_A, maxConcurrent: 0 }];
    });
    const ad = await admin(h);
    const list = await ad.call('GET', '/connections');
    assert.equal(list.status, 200);
    assert.equal(list.body.connections.length, 1);
    const p = list.body.connections[0];
    assert.equal(p.name, 'primary');
    assert.equal(p.credential_set, true);
    assert.equal(p.credential_hint, '6789');
    assert.ok(p.models.some((m: any) => m.id === 'acme-llm-primary' && m.kind === 'chat' && m.context_window === 131072));
    assert.equal((await chat(h, await h.token('dev1'))).status, 200);
    assert.equal(sent(a)[0]!.headers.authorization, `Bearer ${KEY_A}`);
    // Never in a response, an audit record or the database in clear text.
    assert.ok(!JSON.stringify(list.body).includes(KEY_A));
    assert.ok(!JSON.stringify(h.cp.db.prepare('SELECT * FROM audit').all()).includes(KEY_A));
    const enc = (h.cp.db.prepare('SELECT credential_enc FROM llm_connections').get() as any).credential_enc as string;
    assert.match(enc, /^v1\./);
    assert.ok(!enc.includes(KEY_A));
    // Development key file next to the database.
    assert.ok(existsSync(join(dir, 'secret.key')));
    assert.equal(Buffer.from(readFileSync(join(dir, 'secret.key'), 'utf8'), 'base64').length, 32);
    await h.close();

    // A restart with another environment keeps the stored connection (no overwrite), and decrypts its credential.
    h = await startHarness((c) => {
      c.dbPath = join(dir, 'cp.db');
      c.gateway.upstreams = [{ name: 'other', baseUrl: b.url, apiKey: KEY_B, maxConcurrent: 0 }];
    });
    const names = (await (await admin(h)).call('GET', '/connections')).body.connections.map((c: any) => c.name);
    assert.deepEqual(names, ['primary']);
    a.requests.length = 0;
    assert.equal((await chat(h, await h.token('dev1'))).status, 200);
    assert.equal(sent(a)[0]!.headers.authorization, `Bearer ${KEY_A}`);
    assert.equal(sent(b).length, 0);
    await h.close();
  } finally {
    await a.close();
    await b.close();
    rmSync(dir, { recursive: true, force: true });
  }
});

test('connections: production refuses to start without SKOWORK_SECRET_KEY once a connection exists', () => {
  const cfg: Config = loadConfig({ SKOWORK_ENV: 'production', SKOWORK_DB: ':memory:', OPENAI_BASE_URL: 'https://llm.example.test/v1', OPENAI_KEY: 'k-123456789' });
  const db = openDb(':memory:');
  assert.throws(() => new Connections(cfg, db, new Audit(db), () => {}), /SKOWORK_SECRET_KEY is required/);
  // Without any connection it starts, and refuses to store a credential.
  const empty = loadConfig({ SKOWORK_ENV: 'production', SKOWORK_DB: ':memory:' });
  const db2 = openDb(':memory:');
  const c = new Connections(empty, db2, new Audit(db2), () => {});
  assert.equal(c.list().key_available, false);
  c.close();
  // With the key it starts and encrypts under it.
  const keyed = loadConfig({ SKOWORK_ENV: 'production', SKOWORK_DB: ':memory:', OPENAI_BASE_URL: 'https://llm.example.test/v1', OPENAI_KEY: 'k-123456789', SKOWORK_SECRET_KEY: Buffer.alloc(32, 7).toString('base64') });
  const db3 = openDb(':memory:');
  const k = new Connections({ ...keyed, gateway: { ...keyed.gateway, discoveryIntervalMs: 3_600_000 } }, db3, new Audit(db3), () => {});
  assert.equal(k.list().connections.length, 1);
  k.close();
});

test('connections: the server key comes from a passphrase or an existing base64 key; a short value is refused', () => {
  const pass = serverKey('correct horse battery staple');
  assert.equal(pass.length, 32);
  assert.deepEqual(serverKey('correct horse battery staple'), pass, 'the same passphrase gives the same key after a restart');
  assert.notDeepEqual(serverKey('correct horse battery stapler'), pass);
  const raw = Buffer.alloc(32, 7);
  assert.deepEqual(serverKey(raw.toString('base64')), raw, 'an existing base64 key is used as it is');
  assert.throws(() => serverKey('secret'), (e: unknown) => e instanceof StartupError && /SKOWORK_SECRET_KEY must be at least 16 characters/.test(e.message));
  // A production start with a passphrase stores the bootstrap connection.
  const cfg = loadConfig({ SKOWORK_ENV: 'production', SKOWORK_DB: ':memory:', OPENAI_BASE_URL: 'https://llm.example.test/v1', OPENAI_KEY: 'k-123456789', SKOWORK_SECRET_KEY: 'correct horse battery staple' });
  const db = openDb(':memory:');
  const c = new Connections({ ...cfg, gateway: { ...cfg.gateway, discoveryIntervalMs: 3_600_000 } }, db, new Audit(db), () => {});
  assert.equal(c.list().key_available, true);
  assert.equal(c.list().connections.length, 1);
  c.close();
});

test('connections: admin API, discovery, wrong credential, capabilities, disabled, missing and pinned models', async () => {
  const a = await fakeUpstream();
  const b = await fakeUpstream();
  listing(a, ['acme-llm-primary', 'acme-llm-embedding']);
  listing(b, ['acme-llm-primary', 'extra-model']);
  const h = await startHarness((c) => {
    c.gateway.upstreams = [{ name: 'primary', baseUrl: a.url, apiKey: KEY_A, maxConcurrent: 0 }];
  });
  try {
    const ad = await admin(h);
    // Only platform-admins.
    assert.equal((await h.call('GET', '/api/v1/admin/llm/connections', await h.token('dev1'))).status, 403);
    h.cp.db.prepare("INSERT INTO admin_roles (user_name, role) VALUES ('aud1', 'security-auditor')").run();
    assert.equal((await h.call('GET', '/api/v1/admin/llm/connections', await h.token('aud1'))).status, 403);

    // Add: validated, discovered on save, write-only credential.
    assert.equal((await ad.call('POST', '/connections', { name: 'Bad Name', base_url: b.url })).status, 400);
    assert.equal((await ad.call('POST', '/connections', { name: 'x', base_url: 'http://llm.example.com/v1' })).status, 400);
    const added = await ad.call('POST', '/connections', { name: 'backup', base_url: b.url, credential: KEY_B });
    assert.equal(added.status, 200);
    assert.equal(added.body.status, 'ok');
    assert.equal(added.body.credential_hint, '3210');
    assert.deepEqual(added.body.models.map((m: any) => m.id), ['acme-llm-primary', 'extra-model']);
    assert.ok(!JSON.stringify(added.body).includes(KEY_B));
    assert.equal(b.requests.find((r) => r.path.endsWith('/models'))!.headers.authorization, `Bearer ${KEY_B}`);
    const bid = added.body.id;
    assert.equal((await ad.call('POST', '/connections', { name: 'backup', base_url: b.url })).status, 409);

    // Wrong credential: failing with the status, earlier models kept, no upstream text.
    const fixed = b.handler;
    listing(b, [], 401);
    const failing = await ad.call('POST', `/connections/${bid}/test`);
    assert.equal(failing.body.status, 'failing');
    assert.equal(failing.body.status_detail, 'HTTP 401');
    assert.equal(failing.body.models.length, 2);
    assert.ok(!JSON.stringify(failing.body).includes('sk-SECRET'));
    b.handler = fixed;
    assert.equal((await ad.call('POST', `/connections/${bid}/test`)).body.status, 'ok');

    // A model no longer listed stays known and is marked missing.
    const pid = (await ad.call('GET', '/connections')).body.connections[0].id;
    const primary = (await ad.call('POST', `/connections/${pid}/test`)).body;
    assert.equal(primary.models.find((m: any) => m.id === 'acme-llm-secondary').missing, true);
    assert.equal(primary.models.find((m: any) => m.id === 'acme-llm-primary').missing, false);

    // Capabilities come from the stored settings.
    assert.equal((await ad.call('PUT', `/connections/${pid}/models/acme-llm-primary`, { context_window: 'big' })).status, 400);
    assert.equal((await ad.call('PUT', `/connections/${pid}/models/acme-llm-primary`, { context_window: 50000, tools: false })).status, 200);
    const t = await h.token('dev1');
    const main = (await h.call('GET', '/llm/v1/models', t)).body.data.find((m: any) => m.id === 'main');
    assert.equal(main.context_window, 50000);
    assert.equal(main.tools, false);

    // Pinned connection through a Group mapping.
    const pol = (path: string, body: unknown) => h.call(path.endsWith('draft') ? 'PUT' : 'POST', `/api/v1/admin/policy${path}`, ad.t, body);
    await pol('/groups/pinned/draft', { document: { llm: { models: { main: 'backup/acme-llm-primary', small: 'no-such-model' } } }, priority: 10, reason: 'test' });
    await pol('/groups/pinned/publish', { reason: 'test' });
    await h.call('POST', '/api/v1/admin/identity/group-members', ad.t, { user_name: 'pin1', skowork_group: 'pinned' });
    const pin = await h.token('pin1');
    a.requests.length = 0;
    b.requests.length = 0;
    assert.equal((await chat(h, pin)).status, 200);
    assert.equal(sent(a).length, 0);
    assert.equal(sent(b)[0]!.body.model, 'acme-llm-primary');
    assert.equal(sent(b)[0]!.headers.authorization, `Bearer ${KEY_B}`);
    // A bare id goes to the first connection in order.
    assert.equal((await chat(h, t)).status, 200);
    assert.equal(sent(a).length, 1);
    // A mapping that resolves to no known model.
    const missing = await h.call('POST', '/llm/v1/chat/completions', pin, { model: 'small', messages: [{ role: 'user', content: 'hi' }] });
    assert.equal(missing.status, 503);
    assert.equal(missing.body.error.code, 'MODEL_UNAVAILABLE');

    // Disabled connection: 503 and no request reaches it.
    assert.equal((await ad.call('PATCH', `/connections/${bid}`, { enabled: false })).body.enabled, false);
    b.requests.length = 0;
    const off = await chat(h, pin);
    assert.equal(off.status, 503);
    assert.equal(off.body.error.code, 'MODEL_UNAVAILABLE');
    assert.equal(b.requests.length, 0);
    await ad.call('PATCH', `/connections/${bid}`, { enabled: true });
    assert.equal((await chat(h, pin)).status, 200);

    // Editing keeps the credential unless a new one is given.
    await ad.call('PATCH', `/connections/${bid}`, { max_concurrent: 4 });
    assert.equal((await ad.call('GET', `/connections/${bid}`)).body.credential_hint, '3210');

    // Delete in use: refused with the uses named, then confirmed.
    const del = await ad.call('DELETE', `/connections/${bid}`);
    assert.equal(del.status, 409);
    assert.equal(del.body.error.code, 'IN_USE');
    assert.ok(del.body.error.uses.some((u: any) => u.scope === 'group:pinned' && u.role === 'main'));
    assert.equal((await h.call('DELETE', `/api/v1/admin/llm/connections/${bid}?confirm=true`, ad.t)).status, 200);
    assert.equal((await chat(h, pin)).status, 503);

    // Catalog for the Models view pickers.
    const cat = (await ad.call('GET', '/models')).body.models;
    assert.ok(cat.every((m: any) => m.connection === 'primary'));

    // Every change is audited, never with a credential.
    const actions = (h.cp.db.prepare("SELECT action, details FROM audit WHERE action LIKE 'llm.%' ORDER BY id").all() as any[]);
    assert.deepEqual([...new Set(actions.map((r) => r.action))], [
      'llm.connection.bootstrap', 'llm.connection.add', 'llm.connection.test', 'llm.model.update', 'llm.connection.update', 'llm.connection.delete',
    ]);
    const all = JSON.stringify(actions);
    assert.ok(!all.includes(KEY_A) && !all.includes(KEY_B));
  } finally {
    await h.close();
    await a.close();
    await b.close();
  }
});
