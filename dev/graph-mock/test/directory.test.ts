// dev/graph-mock user directory for knowledge enrichment (add-knowledge-graph design D5): /users/{upn} and its manager,
// with department and job title only under User.Read.All.
import { test, after, before } from 'node:test';
import assert from 'node:assert/strict';
import type { GraphMock } from '../src/mock.ts';
import { startMock, as, ALL_SCOPES } from './helpers.ts';

let m: GraphMock;
before(async () => { m = await startMock(); });
after(async () => { await m.close(); });

test('a colleague with department, job title and manager under User.Read.All', async () => {
  const g = await as(m, 'jan', ALL_SCOPES + ' https://graph.microsoft.com/User.Read.All');
  const u = await g.get('/users/eva.schneider%40acme-test.example?$select=displayName,mail,department,jobTitle');
  assert.equal(u.status, 200);
  assert.equal(u.json.displayName, 'Eva Schneider');
  assert.equal(u.json.department, 'Controlling');
  assert.equal(u.json.jobTitle, 'Controllerin');
  const mgr = await g.get('/users/eva.schneider@acme-test.example/manager?$select=displayName,mail');
  assert.equal(mgr.status, 200);
  assert.equal(mgr.json.mail, 'tom.baker@acme-test.example');
  // Tom has no manager; an unknown user is not found.
  assert.equal((await g.get('/users/tom.baker@acme-test.example/manager')).status, 404);
  assert.equal((await g.get('/users/nobody@acme-test.example')).status, 404);
});

test('without User.Read.All: basic profile only, and no manager', async () => {
  const g = await as(m, 'jan', ALL_SCOPES + ' https://graph.microsoft.com/User.ReadBasic.All');
  const u = await g.get('/users/eva.schneider@acme-test.example');
  assert.equal(u.status, 200);
  assert.equal(u.json.department, undefined);
  assert.equal((await g.get('/users/eva.schneider@acme-test.example/manager')).status, 403);
  const none = await as(m, 'jan');
  assert.equal((await none.get('/users/eva.schneider@acme-test.example')).status, 403);
});
