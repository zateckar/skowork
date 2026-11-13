// control-plane/user-messaging (add-user-messaging tasks 1.1, 1.2): directory, Presence, direct and team Chats,
// relay through the push connection, catch-up, idempotent sends, read positions, retention and audit.
import { after, before, describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import { PushClient, register, startHarness, waitUntil, type Harness } from './helpers.ts';

describe('user messaging', () => {
  let h: Harness;
  let admin: string;
  const tok: Record<string, string> = {};
  const inst: Record<string, string> = {};
  const push: Record<string, PushClient> = {};

  const call = (u: string, method: string, path: string, body?: unknown, headers?: Record<string, string>) =>
    h.call(method, `/api/v1/messaging${path}`, tok[u]!, body, headers);
  const mid = () => randomUUID().replace(/-/g, '');

  before(async () => {
    h = await startHarness();
    admin = await h.token('admin1');
    for (const u of ['alice', 'bob', 'carol', 'dave']) {
      tok[u] = await h.token(u, { name: u[0]!.toUpperCase() + u.slice(1) + ' Tester' });
      inst[u] = (await register(h, tok[u]!)).installation_id;
    }
    // carol's Group switches messaging off.
    await h.call('POST', '/api/v1/admin/identity/group-members', admin, { user_name: 'carol', skowork_group: 'quiet' });
    await h.call('PUT', '/api/v1/admin/policy/groups/quiet/draft', admin, { document: { messaging: { mode: 'off' } }, priority: 10, reason: 'test' });
    await h.call('POST', '/api/v1/admin/policy/groups/quiet/publish', admin, { reason: 'test' });
    tok.carol = await h.token('carol');
  });
  after(async () => {
    for (const p of Object.values(push)) p.close();
    await h.close();
  });

  it('directory lists signed-in users with messaging on, with Presence', async () => {
    for (const u of ['alice', 'bob', 'dave']) assert.equal((await call(u, 'GET', '/chats')).status, 200);
    const r = await call('alice', 'GET', '/directory?q=bo');
    assert.equal(r.status, 200);
    assert.deepEqual(r.body.users.map((u: any) => [u.user_name, u.display_name, u.presence]), [['bob', 'Bob Tester', 'offline']]);
    const byDisplay = await call('alice', 'GET', '/directory?q=tester');
    assert.ok(byDisplay.body.users.length >= 3);
    // Messaging off: refused and not listed.
    const off = await call('carol', 'GET', '/chats');
    assert.equal(off.status, 403);
    assert.equal(off.body.error.code, 'MESSAGING_OFF');
    assert.deepEqual((await call('alice', 'GET', '/directory?q=carol')).body.users, []);
    assert.equal((await call('alice', 'POST', '/chats', { kind: 'direct', user: 'carol' })).status, 404);
  });

  it('a direct Chat is unique per pair; messages are relayed and fetched; non-members get 404', async () => {
    push.bob = new PushClient(h, inst.bob!, tok.bob!);
    await push.bob.opened;
    const a = await call('alice', 'POST', '/chats', { kind: 'direct', user: 'bob' });
    assert.equal(a.status, 200);
    const b = await call('bob', 'POST', '/chats', { kind: 'direct', user: 'alice' });
    assert.equal(b.body.id, a.body.id);
    const id = a.body.id;
    const sent = await call('alice', 'POST', `/chats/${id}/messages`, { id: mid(), text: 'Hello **Bob**' });
    assert.equal(sent.status, 200);
    assert.equal(sent.body.seq, 1);
    const pushed = await push.bob.waitFor((m) => m.type === 'chat-message');
    assert.equal(pushed.payload.message.text, 'Hello **Bob**');
    assert.equal(pushed.payload.message.sender_name, 'Alice Tester');
    const list = (await call('bob', 'GET', '/chats')).body.chats;
    assert.equal(list[0].unread, 1);
    assert.equal(list[0].last_message.text, 'Hello **Bob**');
    assert.equal((await call('bob', 'POST', `/chats/${id}/read`, { seq: 1 })).body.read_seq, 1);
    assert.equal((await call('bob', 'GET', '/chats')).body.chats[0].unread, 0);
    assert.equal((await call('dave', 'GET', `/chats/${id}/messages`)).status, 404);
    assert.equal((await call('dave', 'POST', `/chats/${id}/messages`, { id: mid(), text: 'x' })).status, 404);
    // Too long.
    assert.equal((await call('alice', 'POST', `/chats/${id}/messages`, { id: mid(), text: 'x'.repeat(8193) })).status, 413);
  });

  it('a repeated send with the same id is stored and delivered once', async () => {
    const id = (await call('alice', 'POST', '/chats', { kind: 'direct', user: 'bob' })).body.id;
    const m = mid();
    const first = await call('alice', 'POST', `/chats/${id}/messages`, { id: m, text: 'once' });
    const again = await call('alice', 'POST', `/chats/${id}/messages`, { id: m, text: 'once' });
    assert.equal(again.body.seq, first.body.seq);
    const all = (await call('bob', 'GET', `/chats/${id}/messages?after=0`)).body.messages;
    assert.equal(all.filter((x: any) => x.id === m).length, 1);
    await waitUntil(() => push.bob!.messages.filter((x) => x.type === 'chat-message' && x.payload.message.id === m).length === 1);
  });

  it('team Chats: members, rename, add, leave; offline member catches up after its position', async () => {
    const team = await call('alice', 'POST', '/chats', { kind: 'team', name: 'Brakes', members: ['bob', 'dave'] });
    assert.equal(team.status, 200);
    assert.deepEqual(team.body.members.map((m: any) => m.user_name).sort(), ['alice', 'bob', 'dave']);
    const id = team.body.id;
    // dave is offline (no push connection) while two messages arrive.
    await call('alice', 'POST', `/chats/${id}/messages`, { id: mid(), text: 'one' });
    await call('bob', 'POST', `/chats/${id}/messages`, { id: mid(), text: 'two' });
    const caught = (await call('dave', 'GET', `/chats/${id}/messages?after=0`)).body.messages;
    assert.deepEqual(caught.map((m: any) => [m.seq, m.sender, m.text]), [[1, 'alice', 'one'], [2, 'bob', 'two']]);
    assert.deepEqual((await call('dave', 'GET', `/chats/${id}/messages?after=1`)).body.messages.map((m: any) => m.text), ['two']);
    assert.equal((await call('dave', 'PATCH', `/chats/${id}`, { name: 'Brake team' })).body.name, 'Brake team');
    await push.bob!.waitFor((m) => m.type === 'chat-changed' && m.payload.chat === id);
    assert.equal((await call('dave', 'DELETE', `/chats/${id}/members/me`)).status, 200);
    assert.equal((await call('dave', 'GET', `/chats/${id}/messages`)).status, 404);
    // Audit has the Chat events, never the content.
    const rows = h.cp.db.prepare("SELECT action, details FROM audit WHERE category = 'messaging'").all() as any[];
    assert.ok(rows.some((r) => r.action === 'messaging.chat.create'));
    assert.ok(rows.some((r) => r.action === 'messaging.chat.rename'));
    assert.ok(rows.some((r) => r.action === 'messaging.chat.leave'));
    const all = JSON.stringify(h.cp.db.prepare('SELECT * FROM audit').all());
    for (const text of ['Hello **Bob**', 'one', 'two'].map((t) => `"${t}"`)) assert.ok(!all.includes(text), `audit has ${text}`);
    // A team Chat has at most 50 members.
    const many = Array.from({ length: 50 }, (_, i) => `user${i}`);
    assert.equal((await call('alice', 'POST', '/chats', { kind: 'team', name: 'Too big', members: many })).status, 404); // unknown users
  });

  it('Presence: online, away, busy and offline, pushed to Chat members within 5 s', async () => {
    push.alice = new PushClient(h, inst.alice!, tok.alice!);
    await push.alice.opened;
    const seen = (p: string) => push.bob!.waitFor((m) => m.type === 'presence-changed' && m.payload.user_name === 'alice' && m.payload.presence === p, 5000);
    await seen('online');
    await call('alice', 'PUT', '/presence', { state: 'idle' }, { 'x-skowork-installation': inst.alice! });
    await seen('away');
    assert.equal((await call('alice', 'PUT', '/presence', { busy: true })).body.presence, 'busy');
    await seen('busy');
    await call('alice', 'PUT', '/presence', { busy: false, state: 'active' }, { 'x-skowork-installation': inst.alice! });
    await seen('online');
    const started = Date.now();
    push.alice.close();
    delete push.alice;
    await seen('offline');
    assert.ok(Date.now() - started < 5000);
    assert.equal((await call('bob', 'GET', '/directory?q=alice')).body.users[0].presence, 'offline');
  });

  it('retention deletes old messages; admin counts show no content', async () => {
    const id = (await call('alice', 'POST', '/chats', { kind: 'direct', user: 'dave' })).body.id;
    await call('alice', 'POST', `/chats/${id}/messages`, { id: mid(), text: 'old news' });
    h.cp.db.prepare("UPDATE chat_messages SET at = '2020-01-01T00:00:00.000Z' WHERE text = 'old news'").run();
    h.cp.messaging.purge();
    assert.deepEqual((await call('dave', 'GET', `/chats/${id}/messages`)).body.messages, []);
    const counts = await h.call('GET', '/api/v1/admin/messaging/counts', admin);
    assert.equal(counts.status, 200);
    assert.ok(counts.body.chats >= 3);
    assert.ok(!JSON.stringify(counts.body).includes('news'));
  });
});
