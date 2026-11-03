// dev/graph-mock Fault Injection and the control API (task 2.5).
import { test, after, before } from 'node:test';
import assert from 'node:assert/strict';
import type { GraphMock } from '../src/mock.ts';
import { startMock, as, control } from './helpers.ts';

let m: GraphMock;
let g: Awaited<ReturnType<typeof as>>;
before(async () => { m = await startMock(); g = await as(m, 'jan'); });
after(async () => { await m.close(); });

test('throttling on the messages endpoint for the next two requests, then success', async () => {
  const c = control(m);
  await c.post('/faults', { kind: 'throttle', path: '/me/messages', count: 2, retry_after: 7 });
  const a = await g.get('/me/messages?$top=1');
  const b = await g.get('/me/messages?$top=1');
  const ok = await g.get('/me/messages?$top=1');
  assert.equal(a.status, 429);
  assert.equal(a.headers.get('retry-after'), '7');
  assert.equal(a.json.error.code, 'TooManyRequests');
  assert.equal(b.status, 429);
  assert.equal(ok.status, 200);
  // Other endpoints were not affected.
  await c.post('/faults', { kind: 'throttle', path: '/me/events', count: 1 });
  assert.equal((await g.get('/me/messages?$top=1')).status, 200);
});

test('transient server errors, slow responses and malformed payloads', async () => {
  const c = control(m);
  await c.post('/faults', { kind: 'server_error', method: 'GET', path: '/me', count: 1, status: 503 });
  assert.equal((await g.get('/me')).status, 503);
  assert.equal((await g.get('/me')).status, 200);
  await c.post('/faults', { kind: 'slow', path: '/me/mailboxSettings', delay_ms: 300 });
  const t0 = Date.now();
  assert.equal((await g.get('/me/mailboxSettings')).status, 200);
  assert.ok(Date.now() - t0 >= 290);
  await c.post('/faults', { kind: 'malformed', path: '/me/mailFolders' });
  const bad = await g.get('/me/mailFolders');
  assert.equal(bad.status, 200);
  assert.equal(bad.json, undefined, 'the body is not valid JSON');
});

test('the request log records Graph and OAuth calls with faults; reset restores the fixtures', async () => {
  const c = control(m);
  await c.del('/requests');
  await c.post('/faults', { kind: 'throttle', path: '/me/messages', count: 1 });
  await g.get('/me/messages?$top=1');
  const log = (await c.get('/requests?surface=graph')).json;
  assert.equal(log[0].path, '/v1.0/me/messages');
  assert.equal(log[0].status, 429);
  assert.equal(log[0].fault, 'throttle');
  assert.equal(log[0].user, 'jan');

  await c.post('/users/jan/messages', { subject: 'temporary' });
  assert.equal((await g.get('/me/messages?$search="temporary"')).json.value.length, 1);
  const r = await c.post('/reset', {});
  assert.equal(r.json.ok, true);
  assert.equal((await g.get('/me/messages?$search="temporary"')).json.value.length, 0);
  assert.equal((await c.get('/health')).json.fixtures, 'default');
});

test('the control API listens on its own port and is not part of the Graph surface', async () => {
  assert.notEqual(m.controlBase, m.graphBase);
  const r = await fetch(`${m.graphBase}/_mock/health`);
  assert.equal(r.status, 404);
});
