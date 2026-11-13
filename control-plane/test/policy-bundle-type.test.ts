import { test } from 'node:test';
import assert from 'node:assert/strict';
import { decodeProtectedHeader } from 'jose';
import { startHarness } from './helpers.ts';

test('policy: a stored bundle in an earlier type is issued again', async () => {
  const h = await startHarness();
  try {
    const first = await h.call('GET', '/api/v1/policy/bundle', await h.token('dev1'));
    assert.equal(decodeProtectedHeader(first.body.bundle).typ, 'skowork-policy+jwt');

    // Stand-in for a bundle stored before the type changed: the same content under the earlier type.
    const [, payload, signature] = first.body.bundle.split('.');
    const header = Buffer.from(JSON.stringify({ ...decodeProtectedHeader(first.body.bundle), typ: 'workbench-policy+jwt' })).toString('base64url');
    h.cp.db.prepare('UPDATE user_policy_versions SET bundle = ? WHERE user_name = ?').run(`${header}.${payload}.${signature}`, 'dev1');

    const again = await h.call('GET', '/api/v1/policy/bundle', await h.token('dev1'));
    assert.equal(decodeProtectedHeader(again.body.bundle).typ, 'skowork-policy+jwt');
    assert.ok(again.body.version > first.body.version);
  } finally {
    await h.close();
  }
});
