// Policy service API (add-policy-and-sandbox tasks 2.2, 2.4-2.7; control-plane/policy-service).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createLocalJWKSet, decodeProtectedHeader, jwtVerify } from 'jose';
import { PushClient, register, sleep, startHarness, type Harness } from './helpers.ts';
import { rolloutBucket } from '../src/policy/service.ts';

async function setup(h: Harness) {
  const admin = await h.token('admin1');
  const put = (path: string, document: unknown, priority = 10, extra: Record<string, unknown> = {}) =>
    h.call('PUT', `/api/v1/admin/policy${path}/draft`, admin, { document, priority, reason: 'test', ...extra });
  const publish = (path: string, body: Record<string, unknown> = {}) => h.call('POST', `/api/v1/admin/policy${path}/publish`, admin, { reason: 'test', ...body });
  const assign = (user: string, group: string) => h.call('POST', '/api/v1/admin/identity/group-members', admin, { user_name: user, workbench_group: group });
  return { admin, put, publish, assign };
}

async function bundle(h: Harness, user: string, claims: Record<string, unknown> = {}) {
  const r = await h.call('GET', '/api/v1/policy/bundle', await h.token(user, claims));
  return r;
}

async function verify(h: Harness, jws: string) {
  const keys = (await h.call('GET', '/.well-known/policy-keys.json', null)).body;
  return jwtVerify(jws, createLocalJWKSet(keys), { typ: 'workbench-policy+jwt' });
}

test('policy: documents are validated on save, naming the field', async () => {
  const h = await startHarness();
  try {
    const { put } = await setup(h);
    const bad = await put('/groups/developers', { tools: { shell: { mode: 'maybe' } } });
    assert.equal(bad.status, 400);
    assert.equal(bad.body.error.code, 'POLICY_INVALID');
    assert.match(bad.body.error.message, /policy\.tools\.shell\.mode/);
    const unknown = await put('/groups/developers', { tools: { shell: { colour: 'red' } } });
    assert.equal(unknown.status, 400);
    assert.match(unknown.body.error.message, /policy\.tools\.shell\.colour: unknown/);
    const mcp = await put('/groups/developers', { mcp: { servers: [] } });
    assert.equal(mcp.status, 400);
    const cross = await put('/groups/developers', { scheduler: { max_concurrent_runs: 6 }, llm: { max_concurrent_runs: 4 } });
    assert.equal(cross.status, 400);
    assert.equal(cross.body.error.code, 'POLICY_CONFLICT');
    assert.match(cross.body.error.message, /scheduler\.max_concurrent_runs.*llm\.max_concurrent_runs/);
    const gr = await put('/guardrail', { filesystem: { read: ['C:\\**'] } });
    assert.equal(gr.status, 400);
    assert.match(gr.body.error.message, /policy\.filesystem\.read/);
    const ok = await put('/groups/developers', { tools: { shell: { mode: 'sandboxed' } } }, 20);
    assert.equal(ok.status, 200);
    // The Group is defined implicitly by its first document.
    const groups = (await h.call('GET', '/api/v1/admin/policy/groups', (await setup(h)).admin)).body;
    assert.equal(groups[0].name, 'developers');
    assert.equal(groups[0].has_draft, true);
    const schema = (await h.call('GET', '/api/v1/admin/policy/schema', (await setup(h)).admin)).body;
    assert.equal(schema.version, 1);
    assert.equal(schema.schema.$schema, 'https://json-schema.org/draft/2020-12/schema');
  } finally {
    await h.close();
  }
});

test('policy: every known Group is listed, also those with members or role mappings only (improve-usability)', async () => {
  const h = await startHarness();
  try {
    const { admin, put, assign } = await setup(h);
    await assign('alice', 'engineering');
    await h.call('PUT', '/api/v1/admin/identity/role-mappings', admin, { mappings: [{ idp_role: 'sales-staff', workbench_group: 'sales' }], reason: 'test' });
    await h.call('PUT', '/api/v1/admin/policy/groups/defined-only', admin, { description: 'no policy', owners: [] });
    const list = async () => (await h.call('GET', '/api/v1/admin/policy/groups', admin)).body as { name: string; defined: boolean; has_draft: boolean }[];
    assert.deepEqual((await list()).map((g) => [g.name, g.defined]), [['defined-only', true], ['engineering', false], ['everyone', false], ['sales', false]]);
    // A members-only Group can be given a policy directly, which defines it.
    assert.equal((await put('/groups/engineering', { tools: { shell: { mode: 'sandboxed' } } })).status, 200);
    const eng = (await list()).find((g) => g.name === 'engineering')!;
    assert.equal(eng.defined, true);
    assert.equal(eng.has_draft, true);
  } finally {
    await h.close();
  }
});

test('policy: a non-admin is refused and a group-owner manages only their own Groups', async () => {
  const h = await startHarness();
  try {
    const { admin, put } = await setup(h);
    const user = await h.token('dev1');
    assert.equal((await h.call('GET', '/api/v1/admin/policy/groups', user)).status, 403);
    assert.equal((await h.call('PUT', '/api/v1/admin/policy/groups/developers/draft', user, { document: {} })).status, 403);

    await h.call('POST', '/api/v1/admin/identity/admin-roles', admin, { user_name: 'owner1', role: 'group-owner' });
    assert.equal((await h.call('PUT', '/api/v1/admin/policy/groups/developers', admin, { description: 'Developers', owners: ['owner1'] })).status, 200);
    await put('/groups/finance', { tools: { shell: { mode: 'off' } } });
    const owner = await h.token('owner1');
    const own = await h.call('PUT', '/api/v1/admin/policy/groups/developers/draft', owner, { document: { tools: { shell: { mode: 'sandboxed' } } }, priority: 20, reason: 'mine' });
    assert.equal(own.status, 200);
    const foreign = await h.call('PUT', '/api/v1/admin/policy/groups/finance/draft', owner, { document: {}, priority: 1 });
    assert.equal(foreign.status, 403);
    assert.equal((await h.call('PUT', '/api/v1/admin/policy/guardrail/draft', owner, { document: {} })).status, 403);
    assert.equal((await h.call('PUT', '/api/v1/admin/policy/groups/developers', owner, { owners: ['owner1', 'x'] })).status, 403);
    assert.equal((await h.call('POST', '/api/v1/admin/policy/dispatch-rules', owner, { issuer_kind: 'user', issuer: 'x', target_kind: 'any' })).status, 403);

    const audit = h.cp.audit.query({ category: 'policy', limit: 100 });
    assert.ok(audit.some((e: any) => e.action === 'policy.draft.save' && e.outcome === 'ok' && e.user_name === 'owner1' && e.details.scope === 'group:developers'));
    assert.ok(audit.some((e: any) => e.action === 'policy.draft.save' && e.outcome === 'refused' && e.details.scope === 'group:finance'));
  } finally {
    await h.close();
  }
});

test('policy: drafts do not affect users; publish creates the next version and notifies the Group', async () => {
  const h = await startHarness();
  try {
    const { admin, put, publish, assign } = await setup(h);
    await assign('dev1', 'developers');
    const dev = await h.token('dev1');
    const reg = await register(h, dev);
    const push = new PushClient(h, reg.installation_id, dev);
    await push.opened;

    for (let i = 1; i <= 3; i++) {
      await put('/groups/developers', { llm: { max_parallel_tools: i } }, 20);
      assert.equal((await publish('/groups/developers')).body.version, i);
    }
    await push.waitFor((m) => m.type === 'policy-changed');
    const seen = push.messages.length;
    const b3 = (await bundle(h, 'dev1')).body;

    await put('/groups/developers', { llm: { max_parallel_tools: 8 } }, 20);
    await sleep(200);
    assert.equal(push.messages.length, seen, 'saving a draft sends no policy-changed');
    assert.equal((await bundle(h, 'dev1')).body.version, b3.version, 'a draft does not change the effective policy');

    const p = await publish('/groups/developers');
    assert.equal(p.body.version, 4);
    await push.waitFor((m) => m.type === 'policy-changed' && m.id > seen);
    const versions = (await h.call('GET', '/api/v1/admin/policy/groups/developers/versions', admin)).body;
    assert.deepEqual(versions.versions.map((v: any) => v.version), [1, 2, 3, 4]);
    assert.equal(versions.current_version, 4);
    const audit = h.cp.audit.query({ category: 'policy', action: 'policy.publish', limit: 1 })[0] as any;
    assert.deepEqual([audit.details.old_version, audit.details.new_version], [3, 4]);
    const b4 = (await bundle(h, 'dev1')).body;
    assert.ok(b4.version > b3.version);
    const { payload } = await verify(h, b4.bundle);
    assert.equal((payload.policy as any).llm.max_parallel_tools, 8);
    push.close();
  } finally {
    await h.close();
  }
});

test('policy: bundles are signed for the user, reused while unchanged, and verify after key rotation', async () => {
  const h = await startHarness();
  try {
    const { admin, put, publish, assign } = await setup(h);
    await put('/groups/developers', { tools: { rules: { 'browser.*': 'allow' } } }, 20);
    await publish('/groups/developers');
    await assign('dev1', 'developers');

    const a = await bundle(h, 'dev1');
    assert.equal(a.status, 200);
    const { payload, protectedHeader } = await verify(h, a.body.bundle);
    assert.equal(protectedHeader.alg, 'ES256');
    assert.equal(payload.sub, 'dev1');
    assert.equal(payload.uid, 'sub-dev1');
    assert.equal(payload.ver, a.body.version);
    assert.deepEqual(payload.groups, ['developers', 'everyone']);
    assert.equal((payload.src as any)['tools.rules["browser.*"]'], 'developers');
    assert.equal((payload.exp as number) - (payload.iat as number), 7 * 86400);
    assert.equal(new Date((payload.exp as number) * 1000).toISOString(), a.body.expires_at);

    const b = await bundle(h, 'dev1');
    assert.equal(b.body.version, a.body.version);
    assert.equal(b.body.bundle, a.body.bundle);

    // Group membership changed: a new bundle for the new set of Groups, with a higher version.
    await put('/groups/power-users', { llm: { max_parallel_tools: 6 } }, 10);
    await publish('/groups/power-users');
    await assign('dev1', 'power-users');
    const c = await bundle(h, 'dev1');
    assert.ok(c.body.version > b.body.version);
    assert.deepEqual((await verify(h, c.body.bundle)).payload.groups, ['developers', 'everyone', 'power-users']);

    // Rotation: the new and the previous key are both published, so bundles signed with either verify.
    const oldKid = decodeProtectedHeader(c.body.bundle).kid;
    const rot = await h.call('POST', '/api/v1/admin/policy/keys/rotate', admin, {});
    assert.equal(rot.body.retired, oldKid);
    const keys = (await h.call('GET', '/.well-known/policy-keys.json', null)).body.keys.map((k: any) => k.kid);
    assert.ok(keys.includes(oldKid) && keys.includes(rot.body.current));
    await verify(h, c.body.bundle);
    await put('/groups/developers', { tools: { rules: { 'browser.*': 'approve' } } }, 20);
    await publish('/groups/developers');
    const d = await bundle(h, 'dev1');
    assert.equal(decodeProtectedHeader(d.body.bundle).kid, rot.body.current);
    await verify(h, d.body.bundle);

    // A bundle for another user is bound to that user.
    const other = await bundle(h, 'dev2');
    assert.equal((await verify(h, other.body.bundle)).payload.sub, 'dev2');
  } finally {
    await h.close();
  }
});

test('policy: an unchanged bundle is reissued with a new version after half its validity', async () => {
  const h = await startHarness((c) => (c.policy.bundleValidityS = 2));
  try {
    const a = await bundle(h, 'dev1');
    const b = await bundle(h, 'dev1');
    assert.equal(b.body.version, a.body.version);
    await sleep(1100);
    const c = await bundle(h, 'dev1');
    assert.equal(c.body.version, a.body.version + 1);
  } finally {
    await h.close();
  }
});

test('policy: a compile failure keeps the previous bundle while valid, then names the conflict', async () => {
  const h = await startHarness((c) => (c.policy.bundleValidityS = 2));
  try {
    const { put, publish, assign } = await setup(h);
    await put('/groups/a', { scheduler: { max_concurrent_runs: 3 } }, 20);
    await publish('/groups/a');
    await put('/groups/b', { llm: { max_concurrent_runs: 2 } }, 10);
    await publish('/groups/b');
    await assign('dev1', 'a');
    const ok = await bundle(h, 'dev1');
    assert.equal(ok.status, 200);
    // Joining "b" makes the combination break the cross-key rule; no installation showed it, so publish could not catch it.
    await assign('dev1', 'b');
    const stale = await bundle(h, 'dev1');
    assert.equal(stale.status, 200);
    assert.equal(stale.body.stale, true);
    assert.equal(stale.body.version, ok.body.version);
    await sleep(2100);
    const failed = await bundle(h, 'dev1');
    assert.equal(failed.status, 409);
    assert.equal(failed.body.error.code, 'POLICY_COMPILE_FAILED');
    assert.match(failed.body.error.message, /scheduler\.max_concurrent_runs/);
    assert.ok(h.cp.audit.query({ action: 'policy.compile-failed' }).length >= 2);
  } finally {
    await h.close();
  }
});

test('policy: publish is refused when a combination of Groups seen in the fleet would break the cross-key rule', async () => {
  const h = await startHarness();
  try {
    const { put, publish, assign } = await setup(h);
    await put('/groups/power-users', { llm: { max_concurrent_runs: 3 } }, 10);
    await publish('/groups/power-users');
    await assign('dev1', 'developers');
    await assign('dev1', 'power-users');
    await register(h, await h.token('dev1'));

    await put('/groups/developers', { scheduler: { max_concurrent_runs: 5 } }, 20);
    const r = await publish('/groups/developers');
    assert.equal(r.status, 409);
    assert.equal(r.body.error.code, 'POLICY_CONFLICT');
    assert.match(r.body.error.message, /scheduler\.max_concurrent_runs.*llm\.max_concurrent_runs.*developers, everyone, power-users/);
    assert.deepEqual(r.body.error.conflicts[0].keys, ['scheduler.max_concurrent_runs', 'llm.max_concurrent_runs']);
    const v = (await h.call('GET', '/api/v1/admin/policy/groups/developers/versions', (await setup(h)).admin)).body;
    assert.equal(v.current_version, null, 'nothing changed');
    assert.equal(v.versions[0].draft, true);
  } finally {
    await h.close();
  }
});

test('policy: a pilot rollout reaches only pilot members; promote and rollback', async () => {
  const h = await startHarness();
  try {
    const { admin, put, publish, assign } = await setup(h);
    await put('/groups/developers', { tools: { rules: { 'browser.*': 'allow' } } }, 20);
    await publish('/groups/developers');
    for (const u of ['pilot1', 'dev2']) await assign(u, 'developers');
    await assign('pilot1', 'pilot-users');
    const clients: Record<string, PushClient> = {};
    for (const u of ['pilot1', 'dev2']) {
      const t = await h.token(u);
      const reg = await register(h, t);
      clients[u] = new PushClient(h, reg.installation_id, t);
      await clients[u]!.opened;
    }
    const before = { pilot1: (await bundle(h, 'pilot1')).body, dev2: (await bundle(h, 'dev2')).body };

    await put('/groups/developers', { tools: { rules: { 'browser.*': 'approve' } } }, 20);
    const p = await publish('/groups/developers', { rollout: { pilot_group: 'pilot-users' } });
    assert.equal(p.status, 200);
    assert.equal(p.body.notified, 1);
    await clients.pilot1!.waitFor((m) => m.type === 'policy-changed' && m.payload.reason === 'rollout');
    await sleep(200);
    assert.equal(clients.dev2!.messages.filter((m) => m.type === 'policy-changed').length, 0);

    const rule = async (u: string) => ((await verify(h, (await bundle(h, u)).body.bundle)).payload.policy as any).tools.rules['browser.*'];
    assert.equal(await rule('pilot1'), 'approve');
    assert.equal(await rule('dev2'), 'allow');
    assert.equal((await bundle(h, 'dev2')).body.version, before.dev2.version, 'non-selected users keep their bundle');

    // Simulation shows the same result as the pilot client gets.
    const sim = (await h.call('POST', '/api/v1/admin/policy/simulate', admin, { user: 'pilot1' })).body;
    assert.equal(sim.policy.tools.rules['browser.*'], 'approve');

    assert.equal((await publish('/groups/developers')).status, 409, 'publish is refused while a rollout is active');
    const pr = await h.call('POST', '/api/v1/admin/policy/groups/developers/rollout/promote', admin, { reason: 'pilot ok' });
    assert.equal(pr.body.notified, 1);
    await clients.dev2!.waitFor((m) => m.type === 'policy-changed' && m.payload.reason === 'promote');
    assert.equal(await rule('dev2'), 'approve');

    const rb = await h.call('POST', '/api/v1/admin/policy/groups/developers/rollback', admin, { reason: 'too strict' });
    assert.equal(rb.body.version, 3);
    assert.equal(rb.body.copied_version, 1);
    assert.equal(await rule('dev2'), 'allow');
    const v3 = (await h.call('GET', '/api/v1/admin/policy/groups/developers/versions/3', admin)).body;
    const v1 = (await h.call('GET', '/api/v1/admin/policy/groups/developers/versions/1', admin)).body;
    assert.deepEqual(v3.document, v1.document);
    for (const c of Object.values(clients)) c.close();
  } finally {
    await h.close();
  }
});

test('policy: a percentage rollout selects users by the stable hash bucket; cancel returns them', async () => {
  const h = await startHarness();
  try {
    const { admin, put, publish, assign } = await setup(h);
    const users = Array.from({ length: 40 }, (_, i) => `user${i}`);
    for (const u of users) {
      await assign(u, 'developers');
      await register(h, await h.token(u));
    }
    await put('/groups/developers', { llm: { max_parallel_tools: 2 } }, 20);
    await publish('/groups/developers');
    await put('/groups/developers', { llm: { max_parallel_tools: 7 } }, 20);
    const p = await publish('/groups/developers', { rollout: { percent: 30 } });
    const selected = users.filter((u) => rolloutBucket(u) < 30);
    assert.ok(selected.length > 0 && selected.length < users.length);
    assert.equal(p.body.notified, selected.length);
    for (const u of users) {
      const v = ((await verify(h, (await bundle(h, u)).body.bundle)).payload.policy as any).llm.max_parallel_tools;
      assert.equal(v, selected.includes(u) ? 7 : 2, u);
    }
    const c = await h.call('POST', '/api/v1/admin/policy/groups/developers/rollout/cancel', admin, {});
    assert.equal(c.body.notified, selected.length);
    const v = ((await verify(h, (await bundle(h, selected[0]!)).body.bundle)).payload.policy as any).llm.max_parallel_tools;
    assert.equal(v, 2);
  } finally {
    await h.close();
  }
});

test('policy: guardrails apply to everyone at once and show in simulation; drafts can be simulated', async () => {
  const h = await startHarness();
  try {
    const { admin, put, publish, assign } = await setup(h);
    await assign('dev1', 'developers');
    const t = await h.token('dev1');
    const reg = await register(h, t, {});
    await h.call('POST', '/api/v1/fleet/heartbeat', t, { installation_id: reg.installation_id, network_enforcement: 'proxy-only' });
    const push = new PushClient(h, reg.installation_id, t);
    await push.opened;
    await put('/groups/developers', { network: { egress_allow: ['*.example-filesharing.com', 'pypi.org'] }, tools: { shell: { limits: { memory_mb: 8192 } } } }, 20);
    await publish('/groups/developers');
    await put('/guardrail', { network: { egress_deny: ['*.example-filesharing.com'] }, tools: { shell: { limits: { memory_mb: 4096 } } } });
    assert.equal((await publish('/guardrail', { rollout: { percent: 10 } })).status, 400, 'guardrails have no staged rollout');
    const g = await publish('/guardrail');
    assert.equal(g.status, 200);
    await push.waitFor((m) => m.type === 'policy-changed' && !m.payload.group);

    const sim = (await h.call('POST', '/api/v1/admin/policy/simulate', admin, { user: 'dev1' })).body;
    assert.deepEqual(sim.groups, ['developers', 'everyone']);
    assert.deepEqual(sim.policy.network.egress_allow, ['pypi.org']);
    assert.equal(sim.policy.tools.shell.limits.memory_mb, 4096);
    assert.equal(sim.src['tools.shell.limits.memory_mb'], 'guardrail');
    assert.equal(sim.conflicts.length, 2);
    assert.deepEqual(sim.network_enforcement, [{ level: 'proxy-only', installations: 1 }]);

    await put('/groups/developers', { network: { egress_allow: ['pypi.org', 'registry.npmjs.org'] } }, 20);
    const draft = (await h.call('POST', '/api/v1/admin/policy/simulate', admin, { groups: ['developers'], drafts: true })).body;
    assert.deepEqual(draft.policy.network.egress_allow, ['pypi.org', 'registry.npmjs.org']);
    assert.equal((await bundle(h, 'dev1')).status, 200);
    assert.deepEqual(((await verify(h, (await bundle(h, 'dev1')).body.bundle)).payload.policy as any).network.egress_allow, ['pypi.org']);
    push.close();
  } finally {
    await h.close();
  }
});

test('policy: diff lists every added, removed and changed key, and the effect on a user', async () => {
  const h = await startHarness();
  try {
    const { admin, put, publish, assign } = await setup(h);
    await assign('dev1', 'developers');
    await put('/groups/developers', { tools: { shell: { mode: 'off' }, rules: { 'browser.*': 'allow', 'mail.send': 'approve' } }, llm: { max_parallel_tools: 2 } }, 20);
    await publish('/groups/developers');
    await put('/groups/developers', { tools: { shell: { mode: 'sandboxed' }, rules: { 'browser.*': 'approve' } }, telemetry: { level: 'off' } }, 30);
    const d = (await h.call('GET', '/api/v1/admin/policy/groups/developers/diff?from=current&to=draft&user=dev1', admin)).body;
    assert.deepEqual(d.changes.added, [{ path: 'telemetry.level', value: 'off' }]);
    assert.deepEqual(d.changes.removed.map((x: any) => x.path), ['llm.max_parallel_tools', 'tools.rules["mail.send"]']);
    assert.deepEqual(d.changes.changed.map((x: any) => [x.path, x.old, x.new]), [
      ['tools.rules["browser.*"]', 'allow', 'approve'],
      ['tools.shell.mode', 'off', 'sandboxed'],
    ]);
    assert.deepEqual(d.priority, { old: 20, new: 30 });
    assert.equal(d.effective.user, 'dev1');
    assert.equal(d.effective.changes.changed.length, 2);
  } finally {
    await h.close();
  }
});

test('policy: mapping changes notify everyone, direct assignments only the user, and both are audited', async () => {
  const h = await startHarness();
  try {
    const { admin, assign } = await setup(h);
    const clients: PushClient[] = [];
    for (const u of ['dev1', 'dev2']) {
      const t = await h.token(u);
      const reg = await register(h, t);
      const c = new PushClient(h, reg.installation_id, t);
      await c.opened;
      clients.push(c);
    }
    await h.call('PUT', '/api/v1/admin/identity/role-mappings', admin, { mappings: [{ idp_role: 'workbench-developer', workbench_group: 'developers' }] });
    for (const c of clients) await c.waitFor((m) => m.type === 'policy-changed' && m.payload.reason === 'group-mapping');
    const audit = h.cp.audit.query({ action: 'identity.role-mappings.set', limit: 1 })[0] as any;
    assert.deepEqual(audit.details.before, []);
    assert.deepEqual(audit.details.after, [{ idp_role: 'workbench-developer', workbench_group: 'developers' }]);
    assert.equal(audit.user_name, 'admin1');

    await assign('dev2', 'power-users');
    await clients[1]!.waitFor((m) => m.type === 'policy-changed' && m.payload.reason === 'group-assignment');
    await sleep(200);
    assert.equal(clients[0]!.messages.filter((m) => m.payload?.reason === 'group-assignment').length, 0);

    // Mapped roles count for the user's Groups in bundles and in simulation.
    const r = await bundle(h, 'dev3', { roles: ['workbench-developer'] });
    assert.deepEqual((await verify(h, r.body.bundle)).payload.groups, ['developers', 'everyone']);
    for (const c of clients) c.close();
  } finally {
    await h.close();
  }
});

test('policy: dispatch rules authorize only matching issuers and targets, whatever the admin role', async () => {
  const h = await startHarness();
  try {
    const { admin } = await setup(h);
    const svc = h.cp.policy;
    await h.call('POST', '/api/v1/admin/identity/admin-roles', admin, { user_name: 'owner1', role: 'group-owner' });
    await h.call('PUT', '/api/v1/admin/policy/groups/developers', admin, { owners: ['owner1'] });
    const add = (b: Record<string, unknown>) => h.call('POST', '/api/v1/admin/policy/dispatch-rules', admin, b);
    assert.equal((await add({ issuer_kind: 'group-owner', issuer: 'developers', target_kind: 'group', target: 'developers' })).status, 200);
    assert.equal((await add({ issuer_kind: 'service-principal', issuer: 'ci-bot', target_kind: 'group', target: 'developers' })).status, 200);
    assert.equal((await add({ issuer_kind: 'robot', issuer: 'x', target_kind: 'any' })).status, 400);

    const owner = { kind: 'user' as const, userName: 'owner1', adminRoles: ['group-owner'], groups: ['everyone'] };
    const dev = { kind: 'user' as const, userName: 'dev1', groups: ['developers', 'everyone'] };
    const outsider = { kind: 'user' as const, userName: 'fin1', groups: ['everyone', 'finance'] };
    assert.equal(svc.authorizeDispatch(owner, dev).allowed, true);
    assert.equal(svc.authorizeDispatch(owner, outsider).allowed, false);
    assert.equal(svc.authorizeDispatch({ kind: 'service', name: 'ci-bot' }, dev).allowed, true);
    assert.equal(svc.authorizeDispatch({ kind: 'service', name: 'other-bot' }, dev).allowed, false);
    const platform = { kind: 'user' as const, userName: 'admin1', adminRoles: ['platform-admin'], groups: ['everyone'] };
    assert.equal(svc.authorizeDispatch(platform, dev).allowed, false, 'no rule, no dispatch, whatever the admin role');

    const rules = (await h.call('GET', '/api/v1/admin/policy/dispatch-rules', admin)).body;
    assert.equal(rules.length, 2);
    assert.equal((await h.call('DELETE', `/api/v1/admin/policy/dispatch-rules/${rules[1].id}`, admin)).status, 200);
    assert.equal(svc.authorizeDispatch({ kind: 'service', name: 'ci-bot' }, dev).allowed, false);
    // Rules are never part of a bundle.
    const b = await bundle(h, 'dev1');
    assert.doesNotMatch(Buffer.from(b.body.bundle.split('.')[1], 'base64url').toString(), /dispatch|ci-bot/);
  } finally {
    await h.close();
  }
});

test('policy: the gateway takes model roles and budgets from the effective policy', async () => {
  const h = await startHarness();
  try {
    const { put, publish, assign } = await setup(h);
    await put('/groups/restricted', { llm: { models: { main: 'acme-llm-secondary' }, budget: { tokens: 0 } } }, 20);
    await publish('/groups/restricted');
    await assign('lim1', 'restricted');
    const t = await h.token('lim1');
    const models = (await h.call('GET', '/llm/v1/models', t)).body.data.map((m: any) => m.id);
    assert.deepEqual(models, ['main']);
    const vision = await h.call('POST', '/llm/v1/chat/completions', t, { model: 'vision', messages: [{ role: 'user', content: 'hi' }] });
    assert.equal(vision.status, 403);
    assert.equal(vision.body.error.code, 'MODEL_UNAVAILABLE');
    const main = await h.call('POST', '/llm/v1/chat/completions', t, { model: 'main', messages: [{ role: 'user', content: 'hi' }] });
    assert.equal(main.status, 429);
    assert.equal(main.body.error.code, 'QUOTA_EXCEEDED');
    // Users without a mapping keep the server-side defaults.
    const other = (await h.call('GET', '/llm/v1/models', await h.token('dev9'))).body.data.map((m: any) => m.id);
    assert.deepEqual(other, ['main', 'vision', 'small', 'embed', 'rerank', 'image']);
    // A Group whose mapping has no image role cannot generate images.
    const img = await h.call('POST', '/llm/v1/images/generations', t, { model: 'image', prompt: 'a car' });
    assert.equal(img.status, 403);
    assert.equal(img.body.error.code, 'MODEL_UNAVAILABLE');
    assert.equal(img.body.error.reason, 'role not granted');
  } finally {
    await h.close();
  }
});

test('policy: every mutation writes an audit row', async () => {
  const h = await startHarness();
  try {
    const { admin, put, publish } = await setup(h);
    const before = h.cp.audit.query({ category: 'policy', limit: 500 }).length;
    await h.call('PUT', '/api/v1/admin/policy/groups/developers', admin, { description: 'd', owners: [] });
    await put('/groups/developers', { llm: { max_parallel_tools: 2 } }, 20);
    await publish('/groups/developers');
    await put('/groups/developers', { llm: { max_parallel_tools: 3 } }, 20);
    await publish('/groups/developers', { rollout: { percent: 50 } });
    await h.call('POST', '/api/v1/admin/policy/groups/developers/rollout/promote', admin, {});
    await h.call('POST', '/api/v1/admin/policy/groups/developers/rollback', admin, { reason: 'r' });
    await put('/groups/developers', { llm: { max_parallel_tools: 4 } }, 20);
    await h.call('DELETE', '/api/v1/admin/policy/groups/developers/draft', admin);
    await put('/guardrail', { telemetry: { level: 'metadata' } });
    await publish('/guardrail');
    const rule = await h.call('POST', '/api/v1/admin/policy/dispatch-rules', admin, { issuer_kind: 'user', issuer: 'a', target_kind: 'any' });
    await h.call('DELETE', `/api/v1/admin/policy/dispatch-rules/${rule.body.id}`, admin);
    await h.call('POST', '/api/v1/admin/policy/keys/rotate', admin, {});
    const rows = h.cp.audit.query({ category: 'policy', limit: 500 }).slice(0, -before || undefined) as any[];
    const actions = rows.map((r) => r.action).reverse();
    assert.deepEqual(actions, [
      'policy.group.define', 'policy.draft.save', 'policy.publish', 'policy.draft.save', 'policy.publish', 'policy.rollout.promote',
      'policy.rollback', 'policy.draft.save', 'policy.draft.delete', 'policy.draft.save', 'policy.publish',
      'policy.dispatch-rule.add', 'policy.dispatch-rule.remove', 'policy.keys.rotate',
    ]);
    assert.ok(rows.every((r) => r.user_name === 'admin1'));
    const save = rows.find((r) => r.action === 'policy.draft.save')!;
    assert.ok('before' in save.details && 'after' in save.details);
  } finally {
    await h.close();
  }
});
