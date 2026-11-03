// Admin console API (add-telemetry-and-admin tasks 5.1, 5.4, 5.5; control-plane/admin-console).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import { register, startHarness, type Harness } from './helpers.ts';

/** Signed-in tokens for one user of each admin role, plus a user without one. */
async function roles(h: Harness) {
  const admin = await h.token('admin1');
  const grant = (user: string, role: string) => h.call('POST', '/api/v1/admin/identity/admin-roles', admin, { user_name: user, role });
  await grant('auditor1', 'security-auditor');
  await grant('owner1', 'group-owner');
  await grant('reviewer1', 'skill-reviewer');
  await grant('council1', 'works-council-reviewer');
  return {
    admin,
    auditor: await h.token('auditor1'),
    owner: await h.token('owner1'),
    reviewer: await h.token('reviewer1'),
    council: await h.token('council1'),
    user: await h.token('dev1'),
  };
}

const refusals = (h: Harness, user: string) => h.cp.audit.query({ action: 'admin.refused', user }) as any[];

test('admin: a user without an admin role is denied and the attempt is audited', async () => {
  const h = await startHarness();
  try {
    const { user } = await roles(h);
    const me = await h.call('GET', '/api/v1/admin/me', user);
    assert.equal(me.status, 403);
    assert.equal((await h.call('GET', '/api/v1/admin/fleet/installations', user)).status, 403);
    const r = refusals(h, 'dev1');
    assert.equal(r.length, 2);
    assert.equal(r[1].details.path, '/api/v1/admin/me');
    assert.equal(r[1].reason, 'no administrator role');
    assert.equal((await h.call('GET', '/api/v1/admin/me', null)).status, 401);
  } finally {
    await h.close();
  }
});

test('admin: each role sees only its views, and its API is authorized against the same table', async () => {
  const h = await startHarness();
  try {
    const t = await roles(h);
    const views = async (token: string) => (await h.call('GET', '/api/v1/admin/me', token)).body.views;
    assert.deepEqual(await views(t.admin), ['dashboards', 'policy', 'models', 'connections', 'knowledge', 'identity', 'remote', 'mcp', 'skills', 'fleet']);
    assert.deepEqual(await views(t.auditor), ['dashboards', 'policy', 'fleet', 'audit', 'individual', 'access-records']);
    assert.deepEqual(await views(t.owner), ['policy']);
    assert.deepEqual(await views(t.reviewer), ['skills']);
    assert.deepEqual(await views(t.council), ['access-records']);

    const get = async (token: string, path: string) => (await h.call('GET', `/api/v1/admin${path}`, token)).status;
    const reads = ['/dashboards', '/policy/groups', '/identity/role-mappings', '/mcp/servers', '/fleet/installations', '/audit', '/access-records', '/telemetry/stats'];
    const allowed = async (token: string) => {
      const out: string[] = [];
      for (const p of reads) if ((await get(token, p)) === 200) out.push(p);
      return out;
    };
    // platform-admin: all management, but not the audit log or individual-level data.
    assert.deepEqual(await allowed(t.admin), ['/dashboards', '/policy/groups', '/identity/role-mappings', '/mcp/servers', '/fleet/installations', '/telemetry/stats']);
    assert.deepEqual(await allowed(t.auditor), ['/dashboards', '/policy/groups', '/fleet/installations', '/audit', '/access-records']);
    assert.deepEqual(await allowed(t.owner), ['/policy/groups']);
    assert.deepEqual(await allowed(t.reviewer), []);
    // works-council-reviewer: only the access records, and read-only.
    assert.deepEqual(await allowed(t.council), ['/access-records']);
    assert.equal((await h.call('POST', '/api/v1/admin/fleet/users/revoke', t.council, { user_name: 'dev1' })).status, 403);
    assert.equal((await h.call('PUT', '/api/v1/admin/policy/groups/developers/draft', t.council, { document: {} })).status, 403);
    assert.equal((await h.call('POST', '/api/v1/admin/identity/admin-roles', t.council, { user_name: 'x', role: 'platform-admin' })).status, 403);

    // A security-auditor reads but does not manage.
    assert.equal((await h.call('PUT', '/api/v1/admin/identity/role-mappings', t.auditor, { mappings: [] })).status, 403);
    assert.equal((await h.call('PUT', '/api/v1/admin/policy/groups/developers/draft', t.auditor, { document: {} })).status, 403);

    // A group-owner edits only the Groups they own.
    assert.equal((await h.call('PUT', '/api/v1/admin/policy/groups/developers', t.admin, { owners: ['owner1'] })).status, 200);
    assert.equal((await h.call('PUT', '/api/v1/admin/policy/groups/finance', t.admin, { owners: ['someone'] })).status, 200);
    assert.equal((await h.call('PUT', '/api/v1/admin/policy/groups/developers/draft', t.owner, { document: { tools: { shell: { mode: 'sandboxed' } } } })).status, 200);
    assert.equal((await h.call('PUT', '/api/v1/admin/policy/groups/finance/draft', t.owner, { document: { tools: { shell: { mode: 'off' } } } })).status, 403);

    // Every refusal is audited with the path, without the query string.
    const council = refusals(h, 'council1');
    assert.equal(council.length, reads.length - 1 + 3);
    assert.ok(council.every((e) => e.reason === 'role does not allow this action' && !String(e.details.path).includes('?')));
    assert.ok(refusals(h, 'owner1').some((e) => e.details.path === '/api/v1/admin/policy/groups/finance/draft' && e.details.method === 'PUT'));
  } finally {
    await h.close();
  }
});

test('admin: the audit store is append-only', async () => {
  const h = await startHarness();
  try {
    await roles(h);
    const before = h.cp.db.prepare('SELECT * FROM audit ORDER BY id').all();
    assert.ok(before.length > 0);
    assert.throws(() => h.cp.db.prepare("UPDATE audit SET action = 'tampered'").run(), /append-only/);
    assert.throws(() => h.cp.db.prepare('DELETE FROM audit').run(), /append-only/);
    assert.throws(() => h.cp.db.prepare('DELETE FROM audit WHERE id = 1').run(), /append-only/);
    assert.deepEqual(h.cp.db.prepare('SELECT * FROM audit ORDER BY id').all(), before);
  } finally {
    await h.close();
  }
});

test('admin: the audit viewer filters and exports, and the export is audited', async () => {
  const h = await startHarness();
  try {
    const t = await roles(h);
    const all = (await h.call('GET', '/api/v1/admin/audit?action=identity.admin-role.grant', t.auditor)).body;
    assert.equal(all.length, 4);
    const jsonl = await fetch(`${h.base}/api/v1/admin/audit/export?format=jsonl&action=identity.admin-role.grant`, { headers: { authorization: `Bearer ${t.auditor}` } });
    assert.equal(jsonl.status, 200);
    assert.match(jsonl.headers.get('content-disposition') ?? '', /attachment; filename="workbench-audit-.*\.jsonl"/);
    const lines = (await jsonl.text()).trim().split('\n').map((l) => JSON.parse(l));
    assert.deepEqual(lines.map((l) => l.details.user).sort(), ['auditor1', 'council1', 'owner1', 'reviewer1']);
    const csv = await (await fetch(`${h.base}/api/v1/admin/audit/export?format=csv&action=identity.admin-role.grant`, { headers: { authorization: `Bearer ${t.auditor}` } })).text();
    const rows = csv.trim().split('\r\n');
    assert.equal(rows[0], 'id,at,category,action,outcome,subject,user_name,installation_id,source_ip,reason,details');
    assert.equal(rows.length, 5);
    assert.match(rows[1]!, /"identity\.admin-role\.grant"/);
    const exports = h.cp.audit.query({ action: 'audit.export' }) as any[];
    assert.equal(exports.length, 2);
    assert.equal(exports[0].user_name, 'auditor1');
    assert.equal(exports[0].details.format, 'csv');
    assert.equal(exports[0].details.events, 4);
    assert.equal((await h.call('GET', '/api/v1/admin/audit/export?format=xml', t.auditor)).status, 400);
  } finally {
    await h.close();
  }
});

/** A stored telemetry record and crash report of `user` (written as the ingest stores them). */
function seedUserData(h: Harness, user: string) {
  const inst = randomUUID();
  h.cp.db
    .prepare("INSERT INTO telemetry_records (received_at, at, user_name, installation_id, groups, event, attrs) VALUES (?, ?, ?, ?, '[\"everyone\"]', 'tool.call', ?)")
    .run(new Date().toISOString(), new Date().toISOString(), user, inst, JSON.stringify({ 'tool.id': 'web.fetch', 'tool.outcome': 'ok' }));
  return inst;
}

test('admin: individual-level access needs a security-auditor and a reason, and each access is recorded for the works council', async () => {
  const h = await startHarness();
  try {
    const t = await roles(h);
    seedUserData(h, 'dev7');
    const individual = (token: string, query: string) => h.call('GET', `/api/v1/admin/individual/telemetry?${query}`, token);

    // Platform admin denied, with or without a reason.
    assert.equal((await individual(t.admin, 'user=dev7&reason=checking')).status, 403);
    // No reason: refused, nothing recorded, no data.
    const none = await individual(t.auditor, 'user=dev7');
    assert.equal(none.status, 400);
    assert.equal(none.body.error.code, 'REASON_REQUIRED');
    assert.equal(none.body.records, undefined);
    assert.equal((await individual(t.auditor, 'user=dev7&reason=%20%20')).status, 400);
    assert.equal(h.cp.audit.query({ category: 'individual-access' }).length, 0);

    const ok = await individual(t.auditor, `user=dev7&reason=${encodeURIComponent('Incident INC-42: data exfiltration alert')}`);
    assert.equal(ok.status, 200);
    assert.equal(ok.body.records.length, 1);
    assert.equal(ok.body.records[0].attrs['tool.id'], 'web.fetch');

    // Audit events searched by user are individual-level as well.
    assert.equal((await h.call('GET', '/api/v1/admin/audit?user=dev7', t.auditor)).status, 400);
    assert.equal((await h.call('GET', '/api/v1/admin/audit?user=dev7&reason=INC-42', t.auditor)).status, 200);

    // Re-identification of a pseudonym.
    const p = h.cp.telemetry.pseudonym('user', 'dev7');
    const re = await h.call('GET', `/api/v1/admin/individual/pseudonyms/${p}?reason=INC-42`, t.auditor);
    assert.equal(re.status, 200);
    assert.equal(re.body.value, 'dev7');

    // A crash dump of the user: recorded before it is decrypted and sent.
    const dev7 = await h.token('dev7');
    const { installation_id } = await register(h, dev7);
    const sent = await fetch(`${h.base}/api/v1/telemetry/v1/crashes`, {
      method: 'POST', body: new Uint8Array([1, 2, 3, 4]),
      headers: { authorization: `Bearer ${dev7}`, 'content-type': 'application/octet-stream', 'x-workbench-crash-id': 'crash-0001', 'x-workbench-crash-installation_id': installation_id },
    });
    assert.equal(sent.status, 202);
    assert.equal((await h.call('GET', '/api/v1/admin/individual/crashes/crash-0001', t.auditor)).status, 400);
    const dump = await fetch(`${h.base}/api/v1/admin/individual/crashes/crash-0001?reason=INC-42`, { headers: { authorization: `Bearer ${t.auditor}` } });
    assert.deepEqual([...new Uint8Array(await dump.arrayBuffer())], [1, 2, 3, 4]);

    // The works-council-reviewer sees each access with auditor, user, time and reason.
    const records = (await h.call('GET', '/api/v1/admin/access-records', t.council)).body;
    assert.deepEqual(records.map((r: any) => [r.kind, r.auditor, r.user, r.reason]), [
      ['crash-dump', 'auditor1', 'dev7', 'INC-42'],
      ['re-identification', 'auditor1', 'dev7', 'INC-42'],
      ['audit', 'auditor1', 'dev7', 'INC-42'],
      ['telemetry', 'auditor1', 'dev7', 'Incident INC-42: data exfiltration alert'],
    ]);
    assert.ok(records.every((r: any) => !Number.isNaN(Date.parse(r.at))));
    // ... but not the data itself.
    assert.equal((await individual(t.council, 'user=dev7&reason=curious')).status, 403);
  } finally {
    await h.close();
  }
});

test('admin: dashboards aggregate per Group and suppress Groups and cells under 5 users', async () => {
  const h = await startHarness();
  try {
    const t = await roles(h);
    const now = new Date().toISOString();
    const inst = h.cp.db.prepare('INSERT INTO installations (id, subject, user_name, groups, version, first_seen, last_seen) VALUES (?, ?, ?, ?, ?, ?, ?)');
    const rec = h.cp.db.prepare('INSERT INTO telemetry_records (received_at, at, user_name, installation_id, groups, event, attrs) VALUES (?, ?, ?, ?, ?, ?, ?)');
    const seed = (group: string, n: number) => {
      for (let i = 0; i < n; i++) {
        const user = `${group}-user${i}`;
        const groups = JSON.stringify(['everyone', group]);
        const id = randomUUID();
        inst.run(id, `sub-${user}`, user, groups, i < 5 ? '1.2.0' : '1.1.0', now, now);
        rec.run(now, now, user, id, groups, 'run.finished', JSON.stringify({ 'run.outcome': i === 0 ? 'failed' : 'succeeded' }));
        rec.run(now, now, user, id, groups, 'tool.call', JSON.stringify({ 'tool.id': 'files.read', 'tool.outcome': 'ok' }));
        // A tool only two users touch is a cell under the floor.
        if (i < 2) rec.run(now, now, user, id, groups, 'tool.call', JSON.stringify({ 'tool.id': 'rare.tool', 'tool.outcome': 'ok' }));
      }
    };
    seed('big', 6);
    seed('small', 3);
    const d = (await h.call('GET', '/api/v1/admin/dashboards?days=7', t.admin)).body;
    assert.equal(d.k, 5);
    const big = d.groups.find((g: any) => g.group === 'big');
    const small = d.groups.find((g: any) => g.group === 'small');
    assert.deepEqual(small, { group: 'small', suppressed: true });
    assert.equal(big.suppressed, false);
    assert.equal(big.active_users, 6);
    assert.equal(big.active_installations, 6);
    assert.deepEqual(big.runs, { total: 6, failed: 1, cancelled: 0, error_rate: 1 / 6 });
    assert.deepEqual(big.tools, { cells: [{ tool: 'files.read', calls: 6, errors: 0 }], suppressed: 1 });
    assert.deepEqual(big.versions, { cells: [{ version: '1.2.0', installations: 5 }], suppressed: 1 });
    // "everyone" holds all 9 users.
    assert.equal(d.groups.find((g: any) => g.group === 'everyone').active_users, 9);
    // Nothing names a user.
    assert.doesNotMatch(JSON.stringify(d), /user\d/);
    assert.equal((await h.call('GET', '/api/v1/admin/dashboards', t.auditor)).status, 200);
    assert.equal((await h.call('GET', '/api/v1/admin/dashboards', t.owner)).status, 403);
  } finally {
    await h.close();
  }
});
