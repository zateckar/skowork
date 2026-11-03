// Task dispatch and service principals (add-scheduled-and-remote-tasks tasks 5.1, 5.2, 5.4; control-plane/task-dispatch,
// control-plane/identity Service Principal Tokens).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { PushClient, register, waitUntil, startHarness, type Harness } from './helpers.ts';

const CI = 'ci-client';

/** A client-credentials token as Keycloak issues it for a confidential client. */
const spToken = (h: Harness, clientId = CI) =>
  h.issuer.sign({ azp: clientId, client_id: clientId, preferred_username: `service-account-${clientId}`, sid: undefined, email: undefined, name: undefined });

async function setup(h: Harness) {
  const admin = await h.token('admin1');
  const rule = (body: Record<string, unknown>) => h.call('POST', '/api/v1/admin/policy/dispatch-rules', admin, body);
  const assign = (user: string, group: string) => h.call('POST', '/api/v1/admin/identity/group-members', admin, { user_name: user, workbench_group: group });
  const policy = async (group: string, document: unknown) => {
    const put = await h.call('PUT', `/api/v1/admin/policy/groups/${group}/draft`, admin, { document, priority: 10, reason: 'test' });
    assert.equal(put.status, 200, JSON.stringify(put.body));
    const pub = await h.call('POST', `/api/v1/admin/policy/groups/${group}/publish`, admin, { reason: 'test' });
    assert.equal(pub.status, 200, JSON.stringify(pub.body));
  };
  const sp = await h.call('POST', '/api/v1/admin/identity/service-principals', admin, { client_id: CI, name: 'ci', apis: ['tasks'] });
  assert.equal(sp.status, 200, JSON.stringify(sp.body));
  return { admin, rule, assign, policy };
}

/** An installation of a user with its push connection. */
async function laptop(h: Harness, user: string) {
  const token = await h.token(user);
  const reg = await register(h, token);
  const push = new PushClient(h, reg.installation_id, token);
  await push.opened;
  return { id: reg.installation_id, token, push };
}

test('task dispatch: service principal tokens are accepted only for registered clients and their APIs', async () => {
  const h = await startHarness();
  try {
    const { admin, rule } = await setup(h);
    await rule({ issuer_kind: 'service-principal', issuer: 'ci', target_kind: 'user', target: 'dev1' });
    // Registered system calls an API: authenticated as that service principal, with no user identity.
    const ok = await h.call('POST', '/api/v1/tasks', await spToken(h), { target: { user: 'dev1' }, prompt: 'Review the failing build' });
    assert.equal(ok.status, 201, JSON.stringify(ok.body));
    const req = h.cp.audit.query({ action: 'tasks.request', limit: 1 })[0] as any;
    assert.equal(req.user_name ?? null, null);
    assert.equal(req.details.issuer, 'service:ci');
    // Unregistered system: 403, audited, no task.
    const before = (h.cp.db.prepare('SELECT COUNT(*) AS n FROM remote_tasks').get() as { n: number }).n;
    const unknown = await h.call('POST', '/api/v1/tasks', await spToken(h, 'jira-client'), { target: { user: 'dev1' }, prompt: 'x' });
    assert.equal(unknown.status, 403);
    assert.equal((h.cp.db.prepare('SELECT COUNT(*) AS n FROM remote_tasks').get() as { n: number }).n, before);
    assert.match((h.cp.audit.query({ action: 'identity.refused', limit: 1 })[0] as any).reason, /jira-client is not a registered service principal/);
    // A service principal on a user API: 403 and audited.
    const userApi = await h.call('GET', '/api/v1/policy/bundle', await spToken(h));
    assert.equal(userApi.status, 403);
    assert.match((h.cp.audit.query({ action: 'identity.refused', limit: 1 })[0] as any).reason, /service principal ci-client may not call this API/);
    // Retired: refused.
    assert.equal((await h.call('POST', `/api/v1/admin/identity/service-principals/${CI}/retire`, admin)).status, 200);
    assert.equal((await h.call('POST', '/api/v1/tasks', await spToken(h), { target: { user: 'dev1' }, prompt: 'x' })).status, 403);
    const audits = h.cp.audit.query({ category: 'admin', limit: 10 }).map((a: any) => a.action);
    assert.ok(audits.includes('identity.service-principal.register') && audits.includes('identity.service-principal.retire'));
  } finally {
    await h.close();
  }
});

test('task dispatch: create, untrusted delivery, authorization and Group fan-out', async () => {
  const h = await startHarness();
  try {
    const { rule, assign } = await setup(h);
    await rule({ issuer_kind: 'service-principal', issuer: 'ci', target_kind: 'group', target: 'developers' });
    await rule({ issuer_kind: 'service-principal', issuer: 'ci', target_kind: 'user', target: 'jnovak' });
    const dev = await laptop(h, 'jnovak');
    // Create task for a user; a field claiming approval is not delivered.
    const r = await h.call('POST', '/api/v1/tasks', await spToken(h), {
      target: { user: 'jnovak' }, prompt: 'Summarise build 42', pre_approved: true, policy: { tools: { rules: { 'mail.send': 'allow' } } },
      attachments: [{ name: 'log.txt', content_type: 'text/plain', data: Buffer.from('build failed').toString('base64') }],
    });
    assert.equal(r.status, 201, JSON.stringify(r.body));
    assert.equal(r.body.status, 'queued');
    assert.match(r.body.id, /^task_/);
    const push = await dev.push.waitFor((m) => m.type === 'remote-task');
    assert.deepEqual(Object.keys(push.payload).sort(), ['attachments', 'expires_at', 'issuer', 'parent_id', 'prompt', 'provenance', 'skill', 'task_id']);
    assert.deepEqual(push.payload.provenance, { source_kind: 'remote', ref: 'service:ci', untrusted: true });
    assert.deepEqual(push.payload.issuer, { id: 'service:ci', kind: 'service', name: 'ci' });
    assert.equal(push.payload.attachments[0].size, 12);
    // Colleague not permitted: rejected and audited.
    const colleague = await h.call('POST', '/api/v1/tasks', await h.token('pdvorak'), { target: { user: 'finance1' }, prompt: 'Pay this' });
    assert.equal(colleague.status, 403);
    assert.equal(colleague.body.error.code, 'NOT_AUTHORIZED');
    const refused = h.cp.audit.query({ action: 'tasks.request', limit: 1 })[0] as any;
    assert.equal(refused.outcome, 'refused');
    assert.equal(refused.details.issuer, 'user:pdvorak');
    // Group task: one member task per member, each with its own status.
    for (let i = 1; i <= 12; i++) await assign(`member${i}`, 'developers');
    const g = await h.call('POST', '/api/v1/tasks', await spToken(h), { target: { group: 'developers' }, prompt: 'Update your dependencies' });
    assert.equal(g.status, 201, JSON.stringify(g.body));
    assert.equal(g.body.members.length, 12);
    const st = await h.call('GET', `/api/v1/tasks/${g.body.id}`, await spToken(h));
    assert.equal(st.body.members.length, 12);
    assert.ok(st.body.members.every((m: any) => m.status === 'queued' && m.id.startsWith(g.body.id + '.')));
    // Another issuer does not see the task.
    assert.equal((await h.call('GET', `/api/v1/tasks/${g.body.id}`, await h.token('pdvorak'))).status, 404);
    // Bad requests.
    assert.equal((await h.call('POST', '/api/v1/tasks', await spToken(h), { target: { user: 'jnovak' }, prompt: 'x', expires_in_s: 8 * 86400 })).status, 400);
    assert.equal((await h.call('POST', '/api/v1/tasks', await spToken(h), { target: { user: 'jnovak', group: 'developers' }, prompt: 'x' })).status, 400);
    const big = Buffer.alloc(10 * 1024 * 1024 + 1).toString('base64');
    assert.equal((await h.call('POST', '/api/v1/tasks', await spToken(h), { target: { user: 'jnovak' }, prompt: 'x', attachments: [{ name: 'a', data: big }] })).status, 413);
    dev.push.close();
  } finally {
    await h.close();
  }
});

test('task dispatch: two laptops, one claim; status only from the claimant; mapping and transitions', async () => {
  const h = await startHarness();
  try {
    const { rule } = await setup(h);
    await rule({ issuer_kind: 'service-principal', issuer: 'ci', target_kind: 'user', target: 'jnovak' });
    const a = await laptop(h, 'jnovak');
    const b = await laptop(h, 'jnovak');
    const r = await h.call('POST', '/api/v1/tasks', await spToken(h), { target: { user: 'jnovak' }, prompt: 'Run the checks' });
    const pa = await a.push.waitFor((m) => m.type === 'remote-task');
    const pb = await b.push.waitFor((m) => m.type === 'remote-task');
    const member = pa.payload.task_id;
    // Delivered once a push reached an installation.
    assert.equal((await h.call('GET', `/api/v1/tasks/${r.body.id}`, await spToken(h))).body.members[0].status, 'delivered');
    a.push.send({ type: 'remote-task-claim', ref: pa.id });
    const won = await a.push.waitFor((m) => m.type === 'reply' && m.ref === pa.id);
    assert.deepEqual(won.payload, { message: 'remote-task-claim', task_id: member, status: 'awaiting-acceptance' });
    b.push.send({ type: 'remote-task-claim', ref: pb.id });
    const lost = await b.push.waitFor((m) => m.type === 'error' && m.ref === pb.id);
    assert.equal(lost.payload.code, 'CLAIM_REFUSED');
    assert.equal(lost.payload.message, 'remote-task-claim');
    await b.push.waitFor((m) => m.type === 'remote-task-claimed' && m.payload.task_id === member);
    assert.ok(!a.push.messages.some((m) => m.type === 'remote-task-claimed'), 'the winner is not told it lost');
    // The claim is idempotent for the holder (outbox resend).
    a.push.send({ type: 'remote-task-claim', ref: pa.id });
    await waitUntil(() => a.push.messages.filter((m) => m.type === 'reply' && m.ref === pa.id && m.payload.status === 'awaiting-acceptance').length === 2);
    // Status from a non-claiming installation: refused, audited, unchanged.
    b.push.send({ type: 'remote-task-status', ref: pb.id, payload: { state: 'succeeded' } });
    const nc = await b.push.waitFor((m) => m.type === 'error' && m.payload.code === 'NOT_CLAIMANT');
    assert.equal(nc.ref, pb.id);
    assert.equal((h.cp.audit.query({ action: 'tasks.not-claimant', limit: 1 })[0] as any).category, 'security');
    assert.equal((await h.call('GET', `/api/v1/tasks/${r.body.id}`, await spToken(h))).body.members[0].status, 'awaiting-acceptance');
    // Accept, run, dead → failed.
    a.push.send({ type: 'remote-task-accept', ref: pa.id });
    await a.push.waitFor((m) => m.type === 'reply' && m.payload.message === 'remote-task-accept');
    a.push.send({ type: 'remote-task-status', ref: pa.id, payload: { state: 'running' } });
    await a.push.waitFor((m) => m.type === 'reply' && m.payload.status === 'running');
    a.push.send({ type: 'remote-task-status', ref: pa.id, payload: { state: 'dead', result: 'secret output' } });
    await a.push.waitFor((m) => m.type === 'reply' && m.payload.status === 'failed');
    const st = (await h.call('GET', `/api/v1/tasks/${r.body.id}`, await spToken(h))).body.members[0];
    assert.equal(st.status, 'failed');
    assert.equal(st.result, undefined, 'results are not shared without policy');
    assert.deepEqual(st.transitions.map((t: any) => t.status), ['queued', 'delivered', 'claimed', 'awaiting-acceptance', 'accepted', 'running', 'failed']);
    assert.ok(st.transitions.every((t: any) => !Number.isNaN(Date.parse(t.at))));
    // Decline → declined.
    const r2 = await h.call('POST', '/api/v1/tasks', await spToken(h), { target: { user: 'jnovak' }, prompt: 'Another' });
    const p2 = await a.push.waitFor((m) => m.type === 'remote-task' && m.payload.parent_id === r2.body.id);
    a.push.send({ type: 'remote-task-claim', ref: p2.id });
    await a.push.waitFor((m) => m.type === 'reply' && m.ref === p2.id);
    a.push.send({ type: 'remote-task-decline', ref: p2.id });
    await a.push.waitFor((m) => m.type === 'reply' && m.ref === p2.id && m.payload.status === 'declined');
    assert.equal((await h.call('GET', `/api/v1/tasks/${r2.body.id}`, await spToken(h))).body.members[0].status, 'declined');
    a.push.close();
    b.push.close();
  } finally {
    await h.close();
  }
});

test('task dispatch: expiry, cancellation, and a claim after cancellation', async () => {
  const h = await startHarness();
  try {
    const { rule } = await setup(h);
    await rule({ issuer_kind: 'service-principal', issuer: 'ci', target_kind: 'user', target: 'jnovak' });
    const a = await laptop(h, 'jnovak');
    // Expired before delivery: the issuer sees expired, the installations get remote-task-cancelled.
    const r = await h.call('POST', '/api/v1/tasks', await spToken(h), { target: { user: 'jnovak' }, prompt: 'Old', expires_in_s: 60 });
    h.cp.db.prepare('UPDATE remote_tasks SET expires_at = ? WHERE parent_id = ?').run(new Date(Date.now() - 1000).toISOString(), r.body.id);
    assert.equal((await h.call('GET', `/api/v1/tasks/${r.body.id}`, await spToken(h))).body.members[0].status, 'expired');
    await a.push.waitFor((m) => m.type === 'remote-task-cancelled' && m.payload.reason === 'expired');
    // Cancel a running task: the claimant is told, the status becomes cancelled, and later reports do not change it.
    const run = await h.call('POST', '/api/v1/tasks', await spToken(h), { target: { user: 'jnovak' }, prompt: 'Long job' });
    const p = await a.push.waitFor((m) => m.type === 'remote-task' && m.payload.parent_id === run.body.id);
    a.push.send({ type: 'remote-task-claim', ref: p.id });
    await a.push.waitFor((m) => m.type === 'reply' && m.ref === p.id);
    a.push.send({ type: 'remote-task-accept', ref: p.id });
    a.push.send({ type: 'remote-task-status', ref: p.id, payload: { state: 'running' } });
    await a.push.waitFor((m) => m.type === 'reply' && m.ref === p.id && m.payload.status === 'running');
    const c = await h.call('POST', `/api/v1/tasks/${run.body.id}/cancel`, await spToken(h));
    assert.equal(c.body.members[0].status, 'cancelled');
    await a.push.waitFor((m) => m.type === 'remote-task-cancelled' && m.payload.task_id === p.payload.task_id && m.payload.reason === 'cancelled');
    a.push.send({ type: 'remote-task-status', ref: p.id, payload: { state: 'cancelled' } });
    await a.push.waitFor((m) => m.type === 'reply' && m.ref === p.id && m.payload.status === 'cancelled');
    // Claim after cancellation: an offline installation reconnects; it gets both messages and its claim is refused.
    const offlineToken = await h.token('jnovak');
    const off = await register(h, offlineToken);
    const later = await h.call('POST', '/api/v1/tasks', await spToken(h), { target: { user: 'jnovak' }, prompt: 'While you were away' });
    await h.call('POST', `/api/v1/tasks/${later.body.id}/cancel`, await spToken(h));
    const back = new PushClient(h, off.installation_id, offlineToken);
    await back.opened;
    const task = await back.waitFor((m) => m.type === 'remote-task' && m.payload.parent_id === later.body.id);
    await back.waitFor((m) => m.type === 'remote-task-cancelled' && m.payload.task_id === task.payload.task_id);
    back.send({ type: 'remote-task-claim', ref: task.id });
    const refused = await back.waitFor((m) => m.type === 'error' && m.ref === task.id);
    assert.equal(refused.payload.code, 'CLAIM_REFUSED');
    assert.match(refused.payload.reason, /cancelled/);
    a.push.close();
    back.close();
  } finally {
    await h.close();
  }
});

test('task dispatch: trusted issuers, result sharing, callbacks on the allowlist, and rate limits', async () => {
  const h = await startHarness();
  const received: any[] = [];
  const hook = createServer((req, res) => {
    let raw = '';
    req.on('data', (c) => (raw += c));
    req.on('end', () => {
      received.push({ path: req.url, body: JSON.parse(raw) });
      res.writeHead(204).end();
    });
  });
  await new Promise<void>((r) => hook.listen(0, '127.0.0.1', () => r()));
  const port = (hook.address() as { port: number }).port;
  try {
    const { admin, rule, assign, policy } = await setup(h);
    await rule({ issuer_kind: 'service-principal', issuer: 'ci', target_kind: 'user', target: 'jnovak' });
    await assign('jnovak', 'developers');
    await policy('developers', { scheduler: { remote_trusted_issuers: ['service:ci'], remote_share_results: ['service:ci'] } });
    const a = await laptop(h, 'jnovak');
    // Callback URLs: only on the allowlist.
    const off = await h.call('POST', '/api/v1/tasks', await spToken(h), { target: { user: 'jnovak' }, prompt: 'x', callback_url: `http://127.0.0.1:${port}/hooks/ci` });
    assert.equal(off.status, 400);
    assert.equal(off.body.error.code, 'CALLBACK_NOT_ALLOWED');
    assert.equal((await h.call('POST', '/api/v1/admin/tasks/callbacks', admin, { prefix: `http://127.0.0.1:${port}/hooks` })).status, 200);
    assert.equal((await h.call('POST', '/api/v1/tasks', await spToken(h), { target: { user: 'jnovak' }, prompt: 'x', callback_url: `http://127.0.0.1:${port}/other` })).status, 400);
    const r = await h.call('POST', '/api/v1/tasks', await spToken(h), { target: { user: 'jnovak' }, prompt: 'Report', callback_url: `http://127.0.0.1:${port}/hooks/ci` });
    assert.equal(r.status, 201, JSON.stringify(r.body));
    // Trusted issuer: accepted straight after the claim.
    const p = await a.push.waitFor((m) => m.type === 'remote-task' && m.payload.parent_id === r.body.id);
    a.push.send({ type: 'remote-task-claim', ref: p.id });
    assert.equal((await a.push.waitFor((m) => m.type === 'reply' && m.ref === p.id)).payload.status, 'accepted');
    a.push.send({ type: 'remote-task-status', ref: p.id, payload: { state: 'succeeded', result: 'All 12 checks passed.' } });
    await a.push.waitFor((m) => m.type === 'reply' && m.ref === p.id && m.payload.status === 'succeeded');
    const st = (await h.call('GET', `/api/v1/tasks/${r.body.id}`, await spToken(h))).body.members[0];
    assert.equal(st.status, 'succeeded');
    assert.equal(st.result, 'All 12 checks passed.');
    await waitUntil(() => received.length === 1);
    await Promise.all([...h.cp.tasks.pending]);
    assert.deepEqual(received[0], { path: '/hooks/ci', body: { task_id: p.payload.task_id, parent_id: r.body.id, status: 'succeeded', result: 'All 12 checks passed.' } });
    const actions = h.cp.audit.query({ category: 'tasks', limit: 50 }).map((x: any) => x.action);
    for (const act of ['tasks.request', 'tasks.deliver', 'tasks.claim', 'tasks.result-disclosed', 'tasks.status', 'tasks.callback']) assert.ok(actions.includes(act), act);
    // Rate limit: an administrator lowers the limit; further requests are refused and audited.
    assert.equal((await h.call('PUT', '/api/v1/admin/tasks/limits', admin, { issuer: 'service:ci', per_hour: 2 })).status, 200);
    const second = await h.call('POST', '/api/v1/tasks', await spToken(h), { target: { user: 'jnovak' }, prompt: 'x' });
    assert.equal(second.status, 201, 'one request so far in this hour (refused ones do not count)');
    const over = await h.call('POST', '/api/v1/tasks', await spToken(h), { target: { user: 'jnovak' }, prompt: 'x' });
    assert.equal(over.status, 429);
    assert.equal(over.body.error.code, 'RATE_LIMIT');
    assert.ok(Number(over.headers.get('retry-after')) > 0);
    assert.match((h.cp.audit.query({ action: 'tasks.request', limit: 1 })[0] as any).reason, /RATE_LIMIT/);
    a.push.close();
  } finally {
    await h.close();
    await new Promise<void>((r) => hook.close(() => r()));
  }
});
