// MCP catalog (add-core-tools-and-mcp tasks 4.1-4.5, use-idp-tokens-for-mcp tasks 1.1-1.3; control-plane/mcp-catalog,
// control-plane/identity).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { decodeJwt } from 'jose';
import { openDb } from '../src/db.ts';
import { PushClient, fakeMcp, register, sleep, startHarness, type Harness } from './helpers.ts';

const BASE = '/api/v1/admin/mcp';

async function setup(h: Harness) {
  const admin = await h.token('admin1');
  const assign = (user: string, group: string) => h.call('POST', '/api/v1/admin/identity/group-members', admin, { user_name: user, skowork_group: group });
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
    assert.equal(created.body.auth, 'user-token', 'remote entries present the user token by default');
    assert.equal(created.body.audience, undefined);
    assert.deepEqual(created.body.tools.map((t: any) => [t.name, t.status]), [['delete_issue', 'baseline'], ['get_issue', 'baseline'], ['search_issues', 'baseline']]);

    // Discovery presented the administrator's own identity-provider token on every request.
    assert.ok(mcp.authorizations.length >= 2);
    for (const a of mcp.authorizations) assert.equal(a, `Bearer ${admin}`);

    const [jira] = await mcpServers(h, 'dev1');
    assert.deepEqual(jira, {
      id: 'jira-corp', name: 'Jira', kind: 'remote', spec_version: '2025-06-18', url: mcp.url, auth: 'user-token',
      include_tools: [], exclude_tools: ['delete_issue'], risk: { search_issues: 'read' },
    });
    assert.equal((await mcpServers(h, 'pow1')).length, 0, 'another Group does not see the entry');

    // A second entry for power-users; a member of both Groups gets the union.
    mcp.sse = false;
    const conf = await h.call('POST', `${BASE}/servers`, admin, { id: 'confluence-corp', owner: 'it-tools', kind: 'remote', url: mcp.url, auth: 'none', groups: ['power-users'] });
    assert.equal(conf.status, 200);
    assert.deepEqual((await mcpServers(h, 'both1')).map((s) => s.id), ['confluence-corp', 'jira-corp']);
    assert.equal((await mcpServers(h, 'both1'))[0].auth, 'none');
    assert.equal(mcp.authorizations.at(-1), undefined, 'auth none sends no token');

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

test('identity-provider tokens only: no token exchange, no token keys, and re-discovery presents the calling admin token', async () => {
  const mcp = await fakeMcp();
  mcp.tools = [{ name: 'search_issues' }];
  const h = await startHarness(insecure);
  try {
    const { admin } = await setup(h);
    const dev = await h.token('dev1');
    assert.equal((await h.call('POST', '/api/v1/identity/token-exchange', dev, { audience: 'api://jira-corp' })).status, 404);
    assert.equal((await h.call('GET', '/.well-known/skowork-token-keys.json', null)).status, 404);
    assert.equal((await h.call('POST', '/api/v1/admin/identity/token-keys/rotate', admin)).status, 404);

    const old = await h.call('POST', `${BASE}/servers`, admin, { id: 'jira-corp', owner: 'it', kind: 'remote', url: mcp.url, auth: 'audience-token' });
    assert.equal(old.status, 400);
    assert.equal(old.body.error.field, 'auth');

    assert.equal((await h.call('POST', `${BASE}/servers`, admin, { id: 'jira-corp', owner: 'it', kind: 'remote', url: mcp.url })).status, 200);
    await h.call('POST', '/api/v1/admin/identity/admin-roles', admin, { user_name: 'admin2', role: 'platform-admin' });
    const admin2 = await h.token('admin2');
    mcp.authorizations.length = 0;
    const again = await h.call('POST', `${BASE}/servers/jira-corp/discover`, admin2);
    assert.equal(again.body.discovery.status, 'ok', JSON.stringify(again.body));
    assert.ok(mcp.authorizations.length > 0);
    for (const a of mcp.authorizations) assert.equal(a, `Bearer ${admin2}`);
  } finally {
    await h.close();
    await mcp.close();
  }
});

test('migration: stored audience-token entries become user-token without an audience, and the token keys are deleted', () => {
  const dir = mkdtempSync(join(tmpdir(), 'wb-mcp-migration-'));
  try {
    const path = join(dir, 'cp.db');
    let db = openDb(path);
    const entry = { id: 'jira-corp', name: 'Jira', owner: 'it', kind: 'remote', spec_version: '2025-06-18', url: 'https://jira.example.test/mcp', auth: 'audience-token', audience: 'api://jira', args: [], env: {}, include_tools: [], exclude_tools: [], risk: {} };
    db.prepare("INSERT INTO mcp_servers (id, entry, enabled, discovery_status, updated_by, updated_at) VALUES ('jira-corp', ?, 1, 'ok', 'admin1', 'now')").run(JSON.stringify(entry));
    db.prepare("INSERT INTO service_signing_keys (kid, purpose, state, private_jwk, public_jwk, created_at) VALUES ('k', 'mcp-token', 'current', '{}', '{}', 'now')").run();
    // Back to before the use-idp-tokens-for-mcp migration; the later migrations (skill self_approved, team item
    // server_content_hash, usage ttft_ms and cached_tokens, model effort and fallback columns) run again too.
    db.exec('ALTER TABLE skill_versions DROP COLUMN self_approved');
    db.exec('ALTER TABLE team_items DROP COLUMN server_content_hash');
    db.exec('ALTER TABLE llm_usage DROP COLUMN ttft_ms');
    db.exec('ALTER TABLE llm_usage DROP COLUMN cached_tokens');
    db.exec('ALTER TABLE llm_models DROP COLUMN reasoning_effort');
    db.exec('ALTER TABLE llm_models DROP COLUMN fallback_model');
    db.exec('ALTER TABLE llm_models DROP COLUMN fallback_after_s');
    db.exec('ALTER TABLE llm_usage DROP COLUMN answered_by');
    db.exec('ALTER TABLE llm_usage DROP COLUMN fallback');
    // add-api-connectors: the tables of its migration run again too (they are created by it).
    db.exec('DROP TABLE api_connector_tokens; DROP TABLE api_connector_secrets; DROP TABLE api_documents; DROP TABLE api_operations;');
    // Team knowledge publications: its tables are created by its migration as well.
    db.exec(`DROP TABLE knowledge_publication_removals; DROP TABLE knowledge_publication_owners; DROP TABLE knowledge_publication_chunks_fts;
      DROP TABLE knowledge_publication_chunks; DROP TABLE knowledge_publication_evidence; DROP TABLE knowledge_publication_entries;
      DROP TABLE knowledge_publication_groups; DROP TABLE knowledge_publication_content; DROP TABLE knowledge_publication_operations;
      DROP TABLE knowledge_publication_ledger;`);
    // Shared Item Ownership: its columns and indexes.
    db.exec(`DROP INDEX team_items_subject; DROP INDEX team_items_item;
      ALTER TABLE team_items DROP COLUMN owner_issuer; ALTER TABLE team_items DROP COLUMN owner_subject;
      ALTER TABLE team_owners DROP COLUMN issuer; ALTER TABLE team_owners DROP COLUMN subject;`);
    db.prepare('UPDATE schema_version SET version = version - 8').run();
    db.close();
    db = openDb(path);
    const migrated = JSON.parse((db.prepare("SELECT entry FROM mcp_servers WHERE id = 'jira-corp'").get() as { entry: string }).entry);
    assert.equal(migrated.auth, 'user-token');
    assert.equal(migrated.audience, undefined);
    assert.equal(migrated.url, entry.url);
    assert.equal((db.prepare("SELECT COUNT(*) AS n FROM service_signing_keys WHERE purpose = 'mcp-token'").get() as { n: number }).n, 0);
    db.close();
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});