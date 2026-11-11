// control-plane/identity
import { after, before, describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { loadConfig, productionGuard } from '../src/config.ts';
import { CLIENT_ID, TEST_ISSUER, makeIssuer, startHarness, type Harness } from './helpers.ts';

describe('identity: provider trust', () => {
  let h: Harness;
  before(async () => (h = await startHarness()));
  after(() => h.close());

  it('authenticates a valid token as its subject with the user name from preferred_username', async () => {
    const r = await h.call('GET', '/api/v1/identity/whoami', await h.token('dev1'));
    assert.equal(r.status, 200);
    assert.equal(r.body.issuer, TEST_ISSUER);
    assert.equal(r.body.subject, 'sub-dev1');
    assert.equal(r.body.user_name, 'dev1');
  });

  it('refuses a missing token with 401', async () => {
    const r = await h.call('GET', '/api/v1/identity/whoami', null);
    assert.equal(r.status, 401);
    assert.equal(r.body.error.code, 'UNAUTHORIZED');
  });

  it('refuses a token signed by a foreign key and audits it', async () => {
    const other = await makeIssuer('k1');
    const r = await h.call('GET', '/api/v1/identity/whoami', await other.sign({ preferred_username: 'mallory' }));
    assert.equal(r.status, 401);
    const audit = h.cp.audit.query({ action: 'identity.refused' });
    assert.ok(audit.length > 0);
  });

  it('refuses a token from another issuer', async () => {
    const r = await h.call('GET', '/api/v1/identity/whoami', await h.issuer.sign({}, { issuer: 'https://evil.test/realms/x' }));
    assert.equal(r.status, 401);
  });

  it('refuses a token for another audience', async () => {
    const r = await h.call('GET', '/api/v1/identity/whoami', await h.token('dev1', { aud: ['someone-else'] }));
    assert.equal(r.status, 401);
  });

  it('refuses a token issued to a client that is not allowed', async () => {
    const r = await h.call('GET', '/api/v1/identity/whoami', await h.token('dev1', { azp: 'another-app' }));
    assert.equal(r.status, 401);
  });

  it('refuses a token valid for 60 minutes and accepts one valid for 10', async () => {
    assert.equal((await h.call('GET', '/api/v1/identity/whoami', await h.token('dev1', {}, { lifetimeS: 3600 }))).status, 401);
    assert.equal((await h.call('GET', '/api/v1/identity/whoami', await h.token('dev1', {}, { lifetimeS: 600 }))).status, 200);
  });

  it('refuses an expired token but tolerates 30 seconds of clock skew', async () => {
    assert.equal((await h.call('GET', '/api/v1/identity/whoami', await h.token('dev1', {}, { lifetimeS: 300, issuedAgoS: 320 }))).status, 200);
    assert.equal((await h.call('GET', '/api/v1/identity/whoami', await h.token('dev1', {}, { lifetimeS: 300, issuedAgoS: 340 }))).status, 401);
  });

  it('audits the first request of every identity-provider session once', async () => {
    const t = await h.token('newcomer', { sid: 'sid-unique-1' });
    await h.call('GET', '/api/v1/identity/whoami', t);
    await h.call('GET', '/api/v1/identity/whoami', t);
    const rows = h.cp.audit.query({ action: 'identity.session-first-use', user: 'newcomer' });
    assert.equal(rows.length, 1);
  });
});

describe('identity: Groups and admin roles', () => {
  let h: Harness;
  let admin: string;
  before(async () => {
    h = await startHarness();
    admin = await h.token('admin1');
  });
  after(() => h.close());

  it('puts every user in "everyone"', async () => {
    const r = await h.call('GET', '/api/v1/identity/whoami', await h.token('plain'));
    assert.deepEqual(r.body.groups, ['everyone']);
  });

  it('maps token roles to Groups through the role mapping, many-to-many', async () => {
    const put = await h.call('PUT', '/api/v1/admin/identity/role-mappings', admin, {
      mappings: [
        { idp_role: 'WORKBENCH_DEV', workbench_group: 'developers' },
        { idp_role: 'WORKBENCH_DEV', workbench_group: 'region-eu' },
        { idp_role: 'POWER', workbench_group: 'power-users' },
      ],
    });
    assert.equal(put.status, 200);
    const r = await h.call('GET', '/api/v1/identity/whoami', await h.token('dev2', { roles: ['WORKBENCH_DEV', 'POWER', 'unmapped'] }));
    assert.deepEqual(r.body.groups, ['developers', 'everyone', 'power-users', 'region-eu']);
  });

  it('adds Groups assigned directly, whatever the token roles', async () => {
    assert.equal((await h.call('POST', '/api/v1/admin/identity/group-members', admin, { user_name: 'dev3', workbench_group: 'region-de' })).status, 200);
    const r = await h.call('GET', '/api/v1/identity/whoami', await h.token('DEV3'));
    assert.ok(r.body.groups.includes('region-de'));
  });

  it('gives bootstrap administrators platform-admin and refuses the admin API to others', async () => {
    assert.deepEqual((await h.call('GET', '/api/v1/identity/whoami', admin)).body.admin_roles, ['platform-admin']);
    const r = await h.call('GET', '/api/v1/admin/fleet/installations', await h.token('dev1'));
    assert.equal(r.status, 403);
    assert.equal(r.body.error.code, 'FORBIDDEN');
  });

  it('grants admin roles administered in the control plane', async () => {
    assert.equal((await h.call('POST', '/api/v1/admin/identity/admin-roles', admin, { user_name: 'auditor1', role: 'security-auditor' })).status, 200);
    const auditor = await h.token('auditor1');
    assert.equal((await h.call('GET', '/api/v1/admin/audit', auditor)).status, 200);
    // An auditor cannot change role mappings.
    assert.equal((await h.call('PUT', '/api/v1/admin/identity/role-mappings', auditor, { mappings: [] })).status, 403);
  });

  it('records every admin action in the audit log', async () => {
    const rows = h.cp.audit.query({ category: 'admin' });
    assert.ok(rows.some((r: any) => r.action === 'identity.role-mappings.set' && r.user_name === 'admin1'));
  });
});

describe('identity: deny list', () => {
  let h: Harness;
  before(async () => (h = await startHarness()));
  after(() => h.close());

  it('refuses a denied user with 403 DENIED whatever the token', async () => {
    const admin = await h.token('admin1');
    assert.equal((await h.call('POST', '/api/v1/admin/identity/deny-list', admin, { kind: 'user', value: 'bad1', reason: 'left' })).status, 200);
    const r = await h.call('GET', '/api/v1/identity/whoami', await h.token('BAD1'));
    assert.equal(r.status, 403);
    assert.equal(r.body.error.code, 'DENIED');
  });
});

describe('identity: production issuer guard', () => {
  it('refuses an http issuer in production', () => {
    const c = loadConfig({ WORKBENCH_ENV: 'production', OIDC_DISCOVERY_URL: 'http://idp.local/realms/x/.well-known/openid-configuration', OIDC_CLIENT_ID: CLIENT_ID });
    assert.match(productionGuard(c) ?? '', /https/);
  });
  it('refuses a test issuer in production', () => {
    const c = loadConfig({
      WORKBENCH_ENV: 'production', OIDC_DISCOVERY_URL: 'https://idp/realms/x/.well-known/openid-configuration', OIDC_CLIENT_ID: CLIENT_ID,
      WORKBENCH_TEST_ISSUER: 'https://t', WORKBENCH_TEST_ISSUER_JWKS: '{"keys":[]}',
    });
    assert.ok(productionGuard(c));
  });
  it('accepts an https issuer in production', () => {
    const c = loadConfig({
      WORKBENCH_ENV: 'production', OIDC_DISCOVERY_URL: 'https://idp/realms/x/.well-known/openid-configuration', OIDC_CLIENT_ID: CLIENT_ID,
      WORKBENCH_TELEMETRY_SECRET: 'from-the-secret-store',
    });
    assert.equal(productionGuard(c), null);
  });
  it('refuses to start in production without the telemetry secret', () => {
    const c = loadConfig({ WORKBENCH_ENV: 'production', OIDC_DISCOVERY_URL: 'https://idp/realms/x/.well-known/openid-configuration', OIDC_CLIENT_ID: CLIENT_ID });
    assert.match(productionGuard(c) ?? '', /WORKBENCH_TELEMETRY_SECRET/);
  });
  it('starts while the identity provider is unreachable and refuses tokens until it is back', async () => {
    const h = await startHarness((c) => {
      c.identity.discoveryUrl = 'http://127.0.0.1:9/.well-known/openid-configuration';
      c.identity.testIssuer = undefined;
    });
    try {
      assert.equal((await h.call('GET', '/healthz', null)).status, 200);
      assert.equal((await h.call('GET', '/api/v1/identity/whoami', await h.token('dev1'))).status, 401);
    } finally {
      await h.close();
    }
  });
});

describe('identity: admin directory', () => {
  let h: Harness;
  before(async () => (h = await startHarness()));
  after(() => h.close());

  it('lists the users and identity-provider roles the control plane knows, for any administrator', async () => {
    const admin = await h.token('admin1');
    const { register } = await import('./helpers.ts');
    await register(h, await h.token('alice', { roles: ['wb-sales'] }));
    await h.call('POST', '/api/v1/admin/identity/group-members', admin, { user_name: 'bob', workbench_group: 'sales' });
    await h.call('POST', '/api/v1/admin/identity/admin-roles', admin, { user_name: 'rita', role: 'skill-reviewer' });
    await h.call('POST', '/api/v1/admin/identity/deny-list', admin, { kind: 'user', value: 'mallory', reason: 'left' });
    await h.call('PUT', '/api/v1/admin/policy/groups/sales', admin, { description: 'Sales', owners: ['olga'] });
    await h.call('PUT', '/api/v1/admin/identity/role-mappings', admin, { mappings: [{ idp_role: 'wb-dev', workbench_group: 'developers' }] });

    const d = await h.call('GET', '/api/v1/admin/identity/directory', await h.token('rita'));
    assert.equal(d.status, 200, JSON.stringify(d.body));
    const by = Object.fromEntries(d.body.users.map((u: any) => [u.name, u]));
    assert.deepEqual(Object.keys(by).sort(), ['admin1', 'alice', 'bob', 'mallory', 'olga', 'rita']);
    assert.deepEqual(by.alice.sources, ['installation']);
    assert.deepEqual(by.bob.sources, ['member']);
    assert.deepEqual(by.olga.sources, ['owner']);
    assert.equal(by.admin1.bootstrap, true);
    assert.equal(by.rita.bootstrap, false);
    assert.deepEqual(d.body.idp_roles, ['wb-dev', 'wb-sales']);
    assert.equal((await h.call('GET', '/api/v1/admin/identity/directory', await h.token('dev1'))).status, 403);
  });
});

describe('identity: offline-session tokens (add-offline-sign-in 3.1)', () => {
  let h: Harness;
  before(async () => (h = await startHarness()));
  after(() => h.close());

  it('accepts a fresh 5-minute access token from an offline session under the same rules', async () => {
    const t = await h.token('offline1', { sid: 'offline-session-1' }, { lifetimeS: 300 });
    const r = await h.call('GET', '/api/v1/identity/whoami', t);
    assert.equal(r.status, 200);
    assert.equal(r.body.subject, 'sub-offline1');
  });

  it('accepts such a token even when its sid is not known to the control plane', async () => {
    const first = await h.token('offline2', { sid: 'offline-session-2' }, { lifetimeS: 300 });
    const later = await h.token('offline2', { sid: 'offline-session-2-renewed' }, { lifetimeS: 300 });
    assert.equal((await h.call('GET', '/api/v1/identity/whoami', first)).status, 200);
    assert.equal((await h.call('GET', '/api/v1/identity/whoami', later)).status, 200);
  });

  it('refuses a 60-minute access token even when it comes from an offline session', async () => {
    const t = await h.token('offline1', { sid: 'offline-session-1' }, { lifetimeS: 3600 });
    const r = await h.call('GET', '/api/v1/identity/whoami', t);
    assert.equal(r.status, 401);
    assert.equal(r.body.error.code, 'UNAUTHORIZED');
  });

  it('refuses a fresh token from an offline session of a deny-listed installation with 403', async () => {
    const admin = await h.token('admin1');
    const installation = 'inst-offline-revoked';
    assert.equal((await h.call('POST', '/api/v1/admin/identity/deny-list', admin, { kind: 'installation', value: installation, reason: 'lost' })).status, 200);
    const t = await h.token('offline3', { sid: 'offline-session-3' }, { lifetimeS: 300 });
    const r = await h.call('GET', '/api/v1/identity/whoami', t, undefined, { 'x-workbench-installation': installation });
    assert.equal(r.status, 403);
    assert.equal(r.body.error.code, 'DENIED');
  });
});
