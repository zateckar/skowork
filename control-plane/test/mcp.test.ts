// MCP catalog and audience-scoped token exchange (add-core-tools-and-mcp tasks 4.1-4.5; control-plane/mcp-catalog,
// control-plane/identity).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { createLocalJWKSet, decodeJwt, decodeProtectedHeader, jwtVerify } from 'jose';
import { CLIENT_ID, PushClient, fakeMcp, register, sleep, startHarness, type Harness } from './helpers.ts';

const BASE = '/api/v1/admin/mcp';

async function setup(h: Harness) {
  const admin = await h.token('admin1');
  const assign = (user: string, group: string) => h.call('POST', '/api/v1/admin/identity/group-members', admin, { user_name: user, workbench_group: group });
  return { admin, assign };
}

async function mcpServers(h: Harness, user: string): Promise<any[]> {
  const r = await h.call('GET', '/api/v1/policy/bundle', await h.token(user));
  assert.equal(r.status, 200, JSON.stringify(r.body));
  return ((decodeJwt(r.body.bundle).policy as any)?.mcp?.servers ?? []) as any[];
}

const insecure = (c: { mcpCatalog: { allowInsecureHttp: boolean; cacheSeconds: number } }) => {
  c.mcpCatalog.allowInsecureHttp = true;
  c.mcpCatalog.cacheSeconds = 0;
};

test('mcp catalog: only a platform-admin may use the administration API', async () => {
  const h = await startHarness();
  try {
    const { admin } = await setup(h);
    await h.call('POST', '/api/v1/admin/identity/admin-roles', admin, { user_name: 'auditor1', role: 'security-auditor' });
    assert.equal((await h.call('GET', `${BASE}/servers`, await h.token('dev1'))).status, 403);
    assert.equal((await h.call('GET', `${BASE}/servers`, await h.token('auditor1'))).status, 403);
    assert.equal((await h.call('PUT', `${BASE}/packages`, await h.token('auditor1'))).status, 403);
    assert.equal((await h.call('GET', `${BASE}/servers`, null)).status, 401);
    assert.equal((await h.call('GET', `${BASE}/servers`, admin)).status, 200);
  } finally {
    await h.close();
  }
});

test('mcp catalog: entry rules reject a bad id, a plain-HTTP URL and an unknown package', async () => {
  const h = await startHarness();
  try {
    const { admin } = await setup(h);
    const badId = await h.call('POST', `${BASE}/servers`, admin, { id: 'Jira/Corp', owner: 'it', kind: 'remote', url: 'https://jira.example.test/mcp' });
    assert.equal(badId.status, 400);
    assert.equal(badId.body.error.field, 'id');
    assert.match(badId.body.error.message, /Jira\/Corp/);
    const http = await h.call('POST', `${BASE}/servers`, admin, { id: 'jira', owner: 'it', kind: 'remote', url: 'http://jira.example.test/mcp' });
    assert.equal(http.status, 400);
    assert.equal(http.body.error.field, 'url');
    const hash = 'ab'.repeat(32);
    const pkg = await h.call('POST', `${BASE}/servers`, admin, { id: 'local-tool', owner: 'it', kind: 'local', package: hash });
    assert.equal(pkg.status, 400);
    assert.equal(pkg.body.error.field, 'package');
    assert.match(pkg.body.error.message, new RegExp(hash));
    const risk = await h.call('POST', `${BASE}/servers`, admin, { id: 'jira', owner: 'it', kind: 'remote', url: 'https://jira.example.test/mcp', risk: { search: 'harmless' } });
    assert.equal(risk.status, 400);
    assert.equal(risk.body.error.field, 'risk');
    assert.equal((await h.call('GET', `${BASE}/servers`, admin)).body.length, 0);
  } finally {
    await h.close();
  }
});

test('mcp catalog: a remote entry is discovered, versioned, audited and delivered to its Groups', async () => {
  const mcp = await fakeMcp();
  mcp.tools = [{ name: 'search_issues' }, { name: 'get_issue' }, { name: 'delete_issue', annotations: { readOnlyHint: true } }];
  mcp.sse = true;
  const h = await startHarness(insecure);
  try {
    const { admin, assign } = await setup(h);
    await assign('dev1', 'developers');
    await assign('pow1', 'power-users');
    await assign('both1', 'developers');
    await assign('both1', 'power-users');
    const created = await h.call('POST', `${BASE}/servers`, admin, {
      id: 'jira-corp', name: 'Jira', owner: 'it-tools', kind: 'remote', url: mcp.url, groups: ['developers'],
      exclude_tools: ['delete_issue'], risk: { search_issues: 'read' },
    });
    assert.equal(created.status, 200, JSON.stringify(created.body));
    assert.equal(created.body.enabled, true);
    assert.equal(created.body.discovery.status, 'ok');
    assert.equal(created.body.audience, mcp.url, 'the audience defaults to the URL');
    assert.deepEqual(created.body.tools.map((t: any) => [t.name, t.status]), [['delete_issue', 'baseline'], ['get_issue', 'baseline'], ['search_issues', 'baseline']]);

    // Discovery presented a short-lived token for the entry's audience with the discovery subject.
    const disc = decodeJwt(mcp.authorizations[0]!.replace(/^Bearer /, ''));
    assert.equal(disc.sub, 'workbench-catalog-discovery');
    assert.equal(disc.aud, mcp.url);
    assert.ok(disc.exp! - disc.iat! <= 60);

    const [jira] = await mcpServers(h, 'dev1');
    assert.deepEqual(jira, {
      id: 'jira-corp', name: 'Jira', kind: 'remote', spec_version: '2025-06-18', url: mcp.url, auth: 'audience-token', audience: mcp.url,
      include_tools: [], exclude_tools: ['delete_issue'], risk: { search_issues: 'read' },
    });
    assert.equal((await mcpServers(h, 'pow1')).length, 0, 'another Group does not see the entry');

    // A second entry for power-users; a member of both Groups gets the union.
    mcp.sse = false;
    const conf = await h.call('POST', `${BASE}/servers`, admin, { id: 'confluence-corp', owner: 'it-tools', kind: 'remote', url: mcp.url, auth: 'none', groups: ['power-users'] });
    assert.equal(conf.status, 200);
    assert.deepEqual((await mcpServers(h, 'both1')).map((s) => s.id), ['confluence-corp', 'jira-corp']);
    assert.equal((await mcpServers(h, 'both1'))[0].audience, undefined, 'auth none has no audience');

    // Tool Risk Annotations reach the next bundle.
    const annotated = await h.call('PUT', `${BASE}/servers/jira-corp`, admin, { risk: { search_issues: 'read', get_issue: 'read' } });
    assert.equal(annotated.status, 200);
    assert.deepEqual((await mcpServers(h, 'dev1'))[0].risk, { search_issues: 'read', get_issue: 'read' });

    // Disabling removes it from the next bundle.
    assert.equal((await h.call('PUT', `${BASE}/servers/jira-corp`, admin, { enabled: false })).status, 200);
    assert.equal((await mcpServers(h, 'dev1')).length, 0);

    // Every change is a version with an audit record holding before and after.
    const versions = (await h.call('GET', `${BASE}/servers/jira-corp/versions`, admin)).body;
    assert.deepEqual(versions.map((v: any) => v.action), ['mcp.server.update', 'mcp.server.update', 'mcp.server.create']);
    assert.equal(versions[0].snapshot.enabled, false);
    const audit = h.cp.audit.query({ action: 'mcp.server.update' }) as any[];
    assert.equal(audit.length, 2);
    assert.equal(audit[0].user_name, 'admin1');
    assert.equal(audit[0].details.before.enabled, true);
    assert.equal(audit[0].details.after.enabled, false);
    assert.ok(h.cp.catalog.stamp() >= 4);

    assert.equal((await h.call('DELETE', `${BASE}/servers/confluence-corp`, admin)).status, 200);
    assert.equal((await mcpServers(h, 'both1')).length, 0);
    assert.equal((await h.call('GET', `${BASE}/servers/confluence-corp/versions`, admin)).body[0].snapshot, null);
  } finally {
    await h.close();
    await mcp.close();
  }
});

test('mcp catalog: a tool that appears later stays excluded until an administrator includes it', async () => {
  const mcp = await fakeMcp();
  mcp.tools = [{ name: 'search_issues' }, { name: 'get_issue' }];
  const h = await startHarness(insecure);
  try {
    const { admin, assign } = await setup(h);
    await assign('dev1', 'developers');
    await h.call('POST', `${BASE}/servers`, admin, { id: 'jira-corp', owner: 'it', kind: 'remote', url: mcp.url, groups: ['developers'], include_tools: ['search_*', 'get_issue'] });

    mcp.tools = [{ name: 'search_issues' }, { name: 'create_issue' }, { name: 'search_projects' }];
    const again = await h.call('POST', `${BASE}/servers/jira-corp/discover`, admin);
    assert.equal(again.status, 200);
    const status = Object.fromEntries(again.body.tools.map((t: any) => [t.name, [t.status, t.present]]));
    assert.deepEqual(status, {
      search_issues: ['baseline', true], get_issue: ['baseline', false], create_issue: ['pending', true], search_projects: ['pending', true],
    });
    let [jira] = await mcpServers(h, 'dev1');
    assert.deepEqual(jira.exclude_tools, ['create_issue', 'search_projects'], 'new tools are not offered before review');

    const inc = await h.call('PUT', `${BASE}/servers/jira-corp/tools/create_issue`, admin, { decision: 'included' });
    assert.equal(inc.status, 200);
    await h.call('PUT', `${BASE}/servers/jira-corp/tools/search_projects`, admin, { decision: 'excluded' });
    [jira] = await mcpServers(h, 'dev1');
    assert.deepEqual(jira.exclude_tools, ['search_projects']);
    assert.deepEqual(jira.include_tools, ['search_*', 'get_issue', 'create_issue'], 'an included tool joins a non-empty include list');
    const audit = h.cp.audit.query({ action: 'mcp.tool.include' }) as any[];
    assert.equal(audit.length, 1);
    assert.equal((await h.call('PUT', `${BASE}/servers/jira-corp/tools/nope`, admin, { decision: 'included' })).status, 404);
    assert.equal((await h.call('PUT', `${BASE}/servers/jira-corp/tools/create_issue`, admin, { decision: 'maybe' })).status, 400);
  } finally {
    await h.close();
    await mcp.close();
  }
});

test('mcp catalog: an unreachable server is stored disabled and cannot be enabled until discovery succeeds', async () => {
  const mcp = await fakeMcp();
  mcp.tools = [{ name: 'ping' }];
  const url = mcp.url;
  await mcp.close();
  const h = await startHarness((c) => (insecure(c), (c.mcpCatalog.discoveryTimeoutMs = 2000)));
  try {
    const { admin, assign } = await setup(h);
    await assign('dev1', 'developers');
    const created = await h.call('POST', `${BASE}/servers`, admin, { id: 'down', owner: 'it', kind: 'remote', url, groups: ['developers'] });
    assert.equal(created.status, 200);
    assert.equal(created.body.enabled, false);
    assert.equal(created.body.discovery.status, 'failed');
    assert.match(created.body.discovery.error, /could not be reached/);
    const enable = await h.call('PUT', `${BASE}/servers/down`, admin, { enabled: true });
    assert.equal(enable.status, 409);
    assert.equal(enable.body.error.code, 'DISCOVERY_FAILED');
    assert.equal((await h.call('GET', `${BASE}/servers/down`, admin)).body.enabled, false);
    assert.equal((await mcpServers(h, 'dev1')).length, 0);
  } finally {
    await h.close();
  }
});

test('mcp catalog: packages are content-addressed, size-limited and served only to Groups with an enabled entry', async () => {
  const h = await startHarness((c) => (c.mcpCatalog.maxPackageBytes = 1024));
  try {
    const { admin, assign } = await setup(h);
    await assign('dev1', 'developers');
    const bytes = Buffer.from('MZ-pretend-this-is-a-self-contained-server');
    const sha = createHash('sha256').update(bytes).digest('hex');
    const upload = await fetch(`${h.base}${BASE}/packages?name=tool.zip`, { method: 'PUT', headers: { authorization: `Bearer ${admin}`, 'content-type': 'application/octet-stream' }, body: bytes });
    assert.equal(upload.status, 200);
    assert.deepEqual(await upload.json(), { sha256: sha, size: bytes.length, name: 'tool.zip' });
    const big = await fetch(`${h.base}${BASE}/packages`, { method: 'PUT', headers: { authorization: `Bearer ${admin}`, 'content-type': 'application/octet-stream' }, body: Buffer.alloc(2048) });
    assert.equal(big.status, 413);
    const dev = await fetch(`${h.base}${BASE}/packages`, { method: 'PUT', headers: { authorization: `Bearer ${await h.token('dev1')}` }, body: bytes });
    assert.equal(dev.status, 403);

    const created = await h.call('POST', `${BASE}/servers`, admin, {
      id: 'local-tool', owner: 'it', kind: 'local', package: sha.toUpperCase(), args: ['--stdio'], env: { MODE: 'ro' }, groups: ['developers'],
    });
    assert.equal(created.status, 200, JSON.stringify(created.body));
    assert.equal(created.body.discovery.status, 'none');
    const [entry] = await mcpServers(h, 'dev1');
    assert.deepEqual(entry, { id: 'local-tool', name: 'local-tool', kind: 'local', spec_version: '2025-06-18', package: sha, args: ['--stdio'], env: { MODE: 'ro' }, include_tools: [], exclude_tools: [], risk: {} });

    const get = (token: string | null) => fetch(`${h.base}/api/v1/mcp/packages/${sha}`, { headers: token ? { authorization: `Bearer ${token}` } : {} });
    assert.equal((await get(null)).status, 401);
    const other = await get(await h.token('fin1'));
    assert.equal(other.status, 404, 'a package outside the user\'s Groups looks absent');
    const mine = await get(await h.token('dev1'));
    assert.equal(mine.status, 200);
    assert.equal(createHash('sha256').update(Buffer.from(await mine.arrayBuffer())).digest('hex'), sha);

    await h.call('PUT', `${BASE}/servers/local-tool`, admin, { enabled: false });
    assert.equal((await get(await h.token('dev1'))).status, 404, 'a disabled entry no longer grants the package');
  } finally {
    await h.close();
  }
});

test('mcp catalog: a catalog edit reaches a connected client of the affected Groups as policy-changed', async () => {
  const h = await startHarness();
  try {
    const { admin, assign } = await setup(h);
    await assign('dev1', 'developers');
    await assign('fin1', 'finance');
    const dev = await h.token('dev1');
    const fin = await h.token('fin1');
    const devPush = new PushClient(h, (await register(h, dev)).installation_id, dev);
    const finPush = new PushClient(h, (await register(h, fin)).installation_id, fin);
    await devPush.opened;
    await finPush.opened;

    const bytes = Buffer.from('package-v1');
    const sha = createHash('sha256').update(bytes).digest('hex');
    await fetch(`${h.base}${BASE}/packages`, { method: 'PUT', headers: { authorization: `Bearer ${admin}` }, body: bytes });
    await h.call('POST', `${BASE}/servers`, admin, { id: 'local-tool', owner: 'it', kind: 'local', package: sha, groups: ['developers'] });
    await devPush.waitFor((m) => m.type === 'policy-changed' && m.payload.reason === 'mcp-catalog');
    const seen = devPush.messages.length;

    // A new package version for the entry: the developer is notified, finance is not.
    const v2 = Buffer.from('package-v2');
    const sha2 = createHash('sha256').update(v2).digest('hex');
    await fetch(`${h.base}${BASE}/packages`, { method: 'PUT', headers: { authorization: `Bearer ${admin}` }, body: v2 });
    assert.equal((await h.call('PUT', `${BASE}/servers/local-tool`, admin, { package: sha2 })).status, 200);
    await devPush.waitFor((m, i = devPush.messages.indexOf(m)) => i >= seen && m.type === 'policy-changed' && m.payload.reason === 'mcp-catalog');
    assert.equal((await mcpServers(h, 'dev1'))[0].package, sha2);

    // Moving the entry to finance notifies both the Groups before and after.
    const finSeen = finPush.messages.length;
    await h.call('PUT', `${BASE}/servers/local-tool`, admin, { groups: ['finance'] });
    await finPush.waitFor((m, i = finPush.messages.indexOf(m)) => i >= finSeen && m.type === 'policy-changed');
    await sleep(100);
    assert.equal(finPush.messages.filter((m) => m.type === 'policy-changed').length, 1);
    devPush.close();
    finPush.close();
  } finally {
    await h.close();
  }
});

test('token exchange: an assigned audience is granted, others are refused and audited, and key rotation keeps tokens valid', async () => {
  const mcp = await fakeMcp();
  mcp.tools = [{ name: 'search_issues' }];
  const h = await startHarness((c) => (insecure(c), (c.identity.mcpAudiences = ['https://static.example.test'])));
  try {
    const { admin, assign } = await setup(h);
    await assign('dev1', 'developers');
    await h.call('POST', `${BASE}/servers`, admin, { id: 'jira-corp', owner: 'it', kind: 'remote', url: mcp.url, audience: 'api://jira-corp', groups: ['developers'] });
    await h.call('POST', `${BASE}/servers`, admin, { id: 'hr-corp', owner: 'it', kind: 'remote', url: mcp.url, audience: 'api://hr-corp', groups: ['hr'] });

    const presented = await h.token('dev1', {}, { lifetimeS: 200 });
    const presentedExp = decodeJwt(presented).exp!;
    const ex = await h.call('POST', '/api/v1/identity/token-exchange', presented, { audience: 'api://jira-corp' });
    assert.equal(ex.status, 200, JSON.stringify(ex.body));
    const keys = async () => createLocalJWKSet((await h.call('GET', '/.well-known/workbench-token-keys.json', null)).body);
    const wellKnown = (await h.call('GET', '/.well-known/workbench-token-keys.json', null)).body;
    assert.equal(wellKnown.keys.length, 2, 'current and next keys are published');
    const { payload } = await jwtVerify(ex.body.access_token, await keys(), { issuer: h.cp.tokens.issuer, audience: 'api://jira-corp' });
    assert.equal(payload.aud, 'api://jira-corp', 'the server is the only audience');
    assert.equal(payload.sub, decodeJwt(presented).sub);
    assert.ok(payload.exp! <= presentedExp);
    // The keys are not the policy keys.
    const policyKids = (await h.call('GET', '/.well-known/policy-keys.json', null)).body.keys.map((k: any) => k.kid);
    assert.ok(!policyKids.includes(decodeProtectedHeader(ex.body.access_token).kid));

    assert.equal((await h.call('POST', '/api/v1/identity/token-exchange', presented, { audience: 'https://static.example.test' })).status, 200);
    const refused = await h.call('POST', '/api/v1/identity/token-exchange', presented, { audience: 'api://hr-corp' });
    assert.equal(refused.status, 403);
    const refusals = h.cp.audit.query({ action: 'identity.token-exchange', outcome: 'refused' }) as any[];
    assert.equal(refusals.length, 1);
    assert.equal(refusals[0].user_name, 'dev1');
    assert.equal(refusals[0].details.audience, 'api://hr-corp');

    // A disabled entry's audience is no longer granted.
    await h.call('PUT', `${BASE}/servers/jira-corp`, admin, { enabled: false });
    assert.equal((await h.call('POST', '/api/v1/identity/token-exchange', presented, { audience: 'api://jira-corp' })).status, 403);
    await h.call('PUT', `${BASE}/servers/jira-corp`, admin, { enabled: true });

    // A token whose authorized party is not an allowed client: 401 and audited.
    const foreign = await h.token('dev1', { azp: 'some-other-app' });
    const bad = await h.call('POST', '/api/v1/identity/token-exchange', foreign, { audience: 'api://jira-corp' });
    assert.equal(bad.status, 401);
    const idRefusals = h.cp.audit.query({ action: 'identity.refused' }) as any[];
    assert.ok(idRefusals.some((a: any) => /some-other-app/.test(a.reason) && a.details?.path === '/token-exchange'), JSON.stringify(idRefusals));
    assert.notEqual(CLIENT_ID, 'some-other-app');

    // Rotation: the old token still verifies, new tokens use the promoted key.
    const oldKid = decodeProtectedHeader(ex.body.access_token).kid;
    assert.equal((await h.call('POST', '/api/v1/admin/identity/token-keys/rotate', await h.token('dev1'))).status, 403);
    const rot = await h.call('POST', '/api/v1/admin/identity/token-keys/rotate', admin);
    assert.equal(rot.status, 200);
    assert.equal(rot.body.retired, oldKid);
    await jwtVerify(ex.body.access_token, await keys(), { issuer: h.cp.tokens.issuer, audience: 'api://jira-corp' });
    const fresh = await h.call('POST', '/api/v1/identity/token-exchange', presented, { audience: 'api://jira-corp' });
    assert.equal(decodeProtectedHeader(fresh.body.access_token).kid, rot.body.current);
    await jwtVerify(fresh.body.access_token, await keys(), { issuer: h.cp.tokens.issuer, audience: 'api://jira-corp' });
  } finally {
    await h.close();
    await mcp.close();
  }
});
