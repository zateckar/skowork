// control-plane/fleet-registry: registration, heartbeat, push with ack/replay, auth-renew, revocation, minimum versions.
import { after, before, describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import { PushClient, register, sleep, startHarness, waitUntil, type Harness } from './helpers.ts';

describe('fleet: registration and heartbeat', () => {
  let h: Harness;
  let admin: string;
  before(async () => {
    h = await startHarness();
    admin = await h.token('admin1');
  });
  after(() => h.close());

  it('records the installation with the user from the token only, and returns service URLs', async () => {
    const t = await h.token('dev1');
    const r = await register(h, t, { user_principal: 'someone-else' });
    assert.equal(r.status, 200);
    assert.equal(r.body.min_version, '0.0.0');
    assert.equal(r.body.revoked, false);
    assert.match(r.body.services.push, /^ws:\/\/.*\/api\/v1\/fleet\/push$/);
    assert.match(r.body.services.gateway, /\/llm\/v1$/);
    const list = await h.call('GET', `/api/v1/admin/fleet/installations?user=dev1`, admin);
    const inst = list.body.items.find((i: any) => i.id === r.installation_id);
    assert.equal(inst.user_name, 'dev1');
    assert.deepEqual(inst.groups, ['everyone']);
    assert.equal(inst.online, true);
  });

  it('refuses an installation id that belongs to another user', async () => {
    const a = await register(h, await h.token('dev1'));
    const b = await h.call('POST', '/api/v1/fleet/register', await h.token('dev2'), { installation_id: a.installation_id, version: '1.0.0' });
    assert.equal(b.status, 409);
    assert.equal(b.body.error.code, 'INSTALLATION_CONFLICT');
  });

  it('validates the request', async () => {
    const t = await h.token('dev1');
    assert.equal((await h.call('POST', '/api/v1/fleet/register', t, { installation_id: 'x', version: '1.0.0' })).status, 400);
    assert.equal((await h.call('POST', '/api/v1/fleet/register', t, { installation_id: randomUUID(), version: 'one' })).status, 400);
  });

  it('updates Groups at heartbeat when membership changes', async () => {
    const t = await h.token('dev5');
    const r = await register(h, t);
    await h.call('POST', '/api/v1/admin/identity/group-members', admin, { user_name: 'dev5', skowork_group: 'developers' });
    const hb = await h.call('POST', '/api/v1/fleet/heartbeat', await h.token('dev5'), { installation_id: r.installation_id, policy_version: 'p1', status: 'running' });
    assert.equal(hb.status, 200);
    assert.equal(hb.body.heartbeat_interval_s, 300);
    const list = await h.call('GET', `/api/v1/admin/fleet/installations?group=developers`, admin);
    assert.ok(list.body.items.some((i: any) => i.id === r.installation_id && i.policy_version === 'p1' && i.status === 'running'));
  });
});

describe('fleet: push delivery', () => {
  let h: Harness;
  let admin: string;
  before(async () => {
    h = await startHarness();
    admin = await h.token('admin1');
  });
  after(() => h.close());

  it('delivers a pushed message and accepts its ack within 5 seconds', async () => {
    const t = await h.token('dev1');
    const r = await register(h, t);
    const c = new PushClient(h, r.installation_id, t);
    await c.opened;
    const started = Date.now();
    const sent = await h.call('POST', '/api/v1/admin/fleet/push', admin, { type: 'policy-changed', target: { installation_id: r.installation_id }, payload: { version: 'v2' } });
    assert.equal(sent.body.recipients, 1);
    const m = await c.waitFor((x) => x.type === 'policy-changed');
    assert.equal(m.payload.version, 'v2');
    assert.ok(m.id >= 1 && m.issued_at && m.expires_at);
    c.send({ type: 'ack', ref: m.id });
    await waitUntil(() => Boolean((h.cp.db.prepare('SELECT acked_at FROM push_messages WHERE installation_id = ? AND id = ?').get(r.installation_id, m.id) as any)?.acked_at));
    assert.ok(Date.now() - started < 5000);
    c.close();
  });

  it('stores messages for an offline installation and delivers them on reconnect, replaying unacked ones', async () => {
    const t = await h.token('dev2');
    const r = await register(h, t);
    await h.call('POST', '/api/v1/admin/fleet/push', admin, { type: 'policy-changed', target: { user: 'dev2' }, payload: { n: 1 } });
    await h.call('POST', '/api/v1/admin/fleet/push', admin, { type: 'skills-changed', target: { user: 'dev2' }, payload: { n: 2 } });
    const c1 = new PushClient(h, r.installation_id, t);
    await c1.opened;
    const m1 = await c1.waitFor((x) => x.payload.n === 1);
    await c1.waitFor((x) => x.payload.n === 2);
    c1.send({ type: 'ack', ref: m1.id });
    await sleep(100);
    c1.close();
    // Reconnect with last_ack = m1: only the unacknowledged message comes again.
    const c2 = new PushClient(h, r.installation_id, t, m1.id);
    await c2.opened;
    const again = await c2.waitFor((x) => x.payload.n === 2);
    assert.equal(again.type, 'skills-changed');
    assert.ok(!c2.messages.some((x) => x.payload.n === 1));
    c2.close();
  });

  it('fans a Group message out to every member', async () => {
    await h.call('POST', '/api/v1/admin/identity/group-members', admin, { user_name: 'g1', skowork_group: 'team-x' });
    await h.call('POST', '/api/v1/admin/identity/group-members', admin, { user_name: 'g2', skowork_group: 'team-x' });
    const a = await register(h, await h.token('g1'));
    const b = await register(h, await h.token('g2'));
    await register(h, await h.token('outsider'));
    const sent = await h.call('POST', '/api/v1/admin/fleet/push', admin, { type: 'policy-changed', target: { group: 'team-x' }, payload: {} });
    assert.equal(sent.body.recipients, 2);
    for (const inst of [a, b]) {
      const n = h.cp.db.prepare('SELECT COUNT(*) AS n FROM push_messages WHERE installation_id = ?').get(inst.installation_id) as { n: number };
      assert.equal(n.n, 1);
    }
  });

  it('rejects an ack for a message never delivered with NOT_DELIVERED and a security audit event', async () => {
    const t = await h.token('dev3');
    const r = await register(h, t);
    const c = new PushClient(h, r.installation_id, t);
    await c.opened;
    c.send({ type: 'ack', ref: 999 });
    const e = await c.waitFor((x) => x.type === 'error');
    assert.equal(e.payload.code, 'NOT_DELIVERED');
    assert.equal(e.ref, 999);
    assert.ok(h.cp.audit.query({ action: 'fleet.forged-reference' }).length > 0);
    c.send({ type: 'remote-task-claim', ref: 998 });
    assert.equal((await c.waitFor((x) => x.type === 'error' && x.ref === 998)).payload.code, 'NOT_DELIVERED');
    c.send({ type: 'something-else' });
    assert.equal((await c.waitFor((x) => x.payload?.code === 'UNKNOWN_TYPE')).type, 'error');
    c.close();
  });

  it('refuses the handshake for another user’s installation and for unknown installations', async () => {
    const r = await register(h, await h.token('dev4'));
    await assert.rejects(new PushClient(h, r.installation_id, await h.token('intruder')).opened, /handshake 403/);
    await assert.rejects(new PushClient(h, randomUUID(), await h.token('dev4')).opened, /handshake 404/);
    await assert.rejects(new PushClient(h, r.installation_id, 'garbage').opened, /handshake 401/);
  });

  it('replaces an older connection of the same installation with close code 4409', async () => {
    const t = await h.token('dev6');
    const r = await register(h, t);
    const c1 = new PushClient(h, r.installation_id, t);
    await c1.opened;
    const c2 = new PushClient(h, r.installation_id, t);
    await c2.opened;
    assert.equal(await c1.closed, 4409);
    c2.close();
  });
});

describe('fleet: token expiry on the push connection', () => {
  let h: Harness;
  before(async () => {
    h = await startHarness((c) => {
      c.fleet.tokenExpiryGraceS = 0;
      c.fleet.tokenCheckIntervalMs = 100;
      c.identity.clockSkewS = 0;
    });
  });
  after(() => h.close());

  it('keeps the connection open past expiry with auth-renew, and closes it with 4401 without', async () => {
    const t = await h.token('dev1', {}, { lifetimeS: 2 });
    const r = await register(h, t);
    const renewed = new PushClient(h, r.installation_id, t);
    await renewed.opened;
    renewed.send({ type: 'auth-renew', payload: { access_token: await h.token('dev1', {}, { lifetimeS: 300 }) } });
    const t2 = await h.token('dev2', {}, { lifetimeS: 2 });
    const r2 = await register(h, t2);
    const stale = new PushClient(h, r2.installation_id, t2);
    await stale.opened;
    assert.equal(await stale.closed, 4401);
    assert.equal(renewed.closeCode, null);
    renewed.close();
  });

  it('refuses an auth-renew token of a different user and closes the connection', async () => {
    const t = await h.token('dev3');
    const r = await register(h, t);
    const c = new PushClient(h, r.installation_id, t);
    await c.opened;
    c.send({ type: 'auth-renew', payload: { access_token: await h.token('someone') } });
    assert.equal((await c.waitFor((x) => x.type === 'error')).payload.code, 'SUBJECT_MISMATCH');
    assert.equal(await c.closed, 4401);
  });
});

describe('fleet: revocation and minimum version', () => {
  let h: Harness;
  let admin: string;
  before(async () => {
    h = await startHarness();
    admin = await h.token('admin1');
  });
  after(() => h.close());

  it('revokes an installation: revocation push, close 4403, later requests refused', async () => {
    const t = await h.token('dev1');
    const r = await register(h, t);
    const c = new PushClient(h, r.installation_id, t);
    await c.opened;
    assert.equal((await h.call('POST', `/api/v1/admin/fleet/installations/${r.installation_id}/revoke`, admin, { reason: 'lost laptop' })).status, 200);
    assert.equal((await c.waitFor((x) => x.type === 'revocation')).payload.scope, 'installation');
    assert.equal(await c.closed, 4403);
    const hb = await h.call('POST', '/api/v1/fleet/heartbeat', await h.token('dev1'), { installation_id: r.installation_id });
    assert.equal(hb.status, 403);
    assert.equal(hb.body.error.code, 'DENIED');
    // Other installations of the same user are unaffected.
    assert.equal((await register(h, await h.token('dev1'))).status, 200);
  });

  it('revokes a user: every connection closes with 4403 and a new registration is refused', async () => {
    const t = await h.token('dev2');
    const a = await register(h, t);
    const c = new PushClient(h, a.installation_id, t);
    await c.opened;
    assert.equal((await h.call('POST', '/api/v1/admin/fleet/users/revoke', admin, { user: 'dev2' })).status, 200);
    await c.waitFor((x) => x.type === 'revocation');
    assert.equal(await c.closed, 4403);
    const again = await register(h, await h.token('dev2'));
    assert.equal(again.status, 403);
  });

  it('pushes minimum-version to connected clients below it and returns it at registration', async () => {
    const t = await h.token('dev3');
    const old = await register(h, t, { version: '0.9.0', channel: 'pilot' });
    const fresh = await register(h, await h.token('dev4'), { version: '2.0.0', channel: 'pilot' });
    const c = new PushClient(h, old.installation_id, t);
    await c.opened;
    const set = await h.call('PUT', '/api/v1/admin/fleet/min-versions/pilot', admin, { version: '1.0.0' });
    assert.equal(set.body.pushed, 1);
    const m = await c.waitFor((x) => x.type === 'minimum-version');
    assert.deepEqual(m.payload, { channel: 'pilot', min_version: '1.0.0' });
    assert.equal(h.cp.db.prepare('SELECT COUNT(*) AS n FROM push_messages WHERE installation_id = ?').get(fresh.installation_id)!.n, 0);
    const late = await register(h, await h.token('dev5'), { version: '0.5.0', channel: 'pilot' });
    assert.equal(late.body.min_version, '1.0.0');
    c.close();
  });
});
