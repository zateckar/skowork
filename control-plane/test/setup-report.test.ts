// control-plane/admin-console Console Entry Point and Deployment Setup Report (improve-deployment-guidance).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { loadConfig } from '../src/config.ts';
import { createControlPlane } from '../src/app.ts';
import { setupReport } from '../src/setup-report.ts';

const guarded = {
  WORKBENCH_ENV: 'production',
  OIDC_DISCOVERY_URL: 'https://idp.example.com/realms/r/.well-known/openid-configuration',
  WORKBENCH_TELEMETRY_SECRET: 'secret',
};

test('setup report: only the guarded settings warn about every missing setting by name', () => {
  const r = setupReport(loadConfig(guarded), { consoleBuilt: true, port: 8080 });
  assert.deepEqual(r.info, ['admin console: http://localhost:8080/admin/']);
  for (const name of ['OIDC_CLIENT_ID', 'WORKBENCH_PUBLIC_BASE_URL', 'WORKBENCH_BOOTSTRAP_ADMINS', 'WORKBENCH_SECRET_KEY', 'OPENAI_BASE_URL'])
    assert.ok(r.warnings.some((w) => w.includes(name)), `warning for ${name}`);
  assert.equal(r.warnings.length, 5);
});

test('setup report: a complete setup gives the console URL and no warnings', () => {
  const r = setupReport(
    loadConfig({
      ...guarded,
      OIDC_CLIENT_ID: 'workbench-desktop',
      WORKBENCH_PUBLIC_BASE_URL: 'https://cp.example.com/',
      WORKBENCH_BOOTSTRAP_ADMINS: 'alice',
      WORKBENCH_SECRET_KEY: Buffer.alloc(32, 1).toString('base64'),
      OPENAI_BASE_URL: 'https://llm.example.com/v1',
    }),
    { consoleBuilt: true, port: 8080 },
  );
  assert.deepEqual(r, { info: ['admin console: https://cp.example.com/admin/'], warnings: [] });
});

test('setup report: a missing console build is a warning', () => {
  const r = setupReport(loadConfig(guarded), { consoleBuilt: false, port: 8080 });
  assert.deepEqual(r.info, []);
  assert.ok(r.warnings[0]?.includes('admin console is not built'));
});

test('console entry point: the root redirects to /admin/ when the console is built, and is not found otherwise', async () => {
  const dist = mkdtempSync(join(tmpdir(), 'wb-admin-'));
  writeFileSync(join(dist, 'index.html'), '<!doctype html><title>Workbench admin</title>');
  const cfg = loadConfig({ WORKBENCH_ENV: 'test', WORKBENCH_DB: ':memory:', OIDC_CLIENT_ID: 'c' });
  const built = createControlPlane(cfg, () => {}, { adminDist: dist });
  const missing = createControlPlane(cfg, () => {}, { adminDist: join(dist, 'none') });
  try {
    const port = await built.listen(0, '127.0.0.1');
    const root = await fetch(`http://127.0.0.1:${port}/`, { redirect: 'manual' });
    assert.equal(root.status, 302);
    assert.equal(root.headers.get('location'), '/admin/');
    // A sign-in response sent to the root keeps its code and state.
    const signIn = await fetch(`http://127.0.0.1:${port}/?state=s1&session_state=x&iss=https%3A%2F%2Fidp&code=c1`, { redirect: 'manual' });
    assert.equal(signIn.status, 302);
    assert.equal(signIn.headers.get('location'), '/admin/?state=s1&session_state=x&iss=https%3A%2F%2Fidp&code=c1');
    const page = await fetch(`http://127.0.0.1:${port}/admin/`);
    assert.equal(page.status, 200);
    assert.match(await page.text(), /Workbench admin/);
    assert.equal(built.consoleBuilt, true);

    const port2 = await missing.listen(0, '127.0.0.1');
    assert.equal((await fetch(`http://127.0.0.1:${port2}/`, { redirect: 'manual' })).status, 404);
    assert.equal(missing.consoleBuilt, false);
  } finally {
    await built.close();
    await missing.close();
    rmSync(dist, { recursive: true, force: true });
  }
});
