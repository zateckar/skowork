// control-plane/team-knowledge (add-knowledge-sharing tasks 1.1, 1.2): publication with Group and policy checks,
// size limits, search isolation between Groups, unsharing, revoked owners, admin removal, and the policy keys.
import { after, before, describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { chunkText } from '../src/knowledge/team.ts';
import { fakeUpstream, json, startHarness, type FakeUpstream, type Harness } from './helpers.ts';

/** Bag-of-words vectors, so similar texts get similar embeddings. */
function embed(text: string): number[] {
  const v = new Array(64).fill(0);
  for (const w of text.toLowerCase().match(/[\p{L}\p{N}]+/gu) ?? []) v[createHash('sha1').update(w).digest()[0]! % 64] += 1;
  return v;
}

const hash = (s: string) => createHash('sha256').update(s).digest('hex');

describe('team knowledge', () => {
  let up: FakeUpstream;
  let h: Harness;
  let admin: string;
  const tok: Record<string, string> = {};

  before(async () => {
    up = await fakeUpstream();
    const base = up.handler;
    up.handler = (path, body, res) => {
      if (!path.endsWith('/embeddings')) return base(path, body, res);
      const inputs: string[] = Array.isArray(body.input) ? body.input : [body.input];
      json(res, 200, { data: inputs.map((t, index) => ({ index, embedding: embed(t) })), usage: { prompt_tokens: 3 * inputs.length, total_tokens: 3 * inputs.length } });
    };
    h = await startHarness((c) => {
      c.gateway.upstreams = [{ name: 'a', baseUrl: up.url, apiKey: 'k-0123456789', maxConcurrent: 0 }];
      c.teamKnowledge.maxItemBytes = 20_000;
      c.teamKnowledge.quotaBytes = 30_000;
    });
    admin = await h.token('admin1');
    const member = (u: string, g: string) => h.call('POST', '/api/v1/admin/identity/group-members', admin, { user_name: u, workbench_group: g });
    await member('alice', 'engineering');
    await member('dave', 'engineering');
    await member('carol', 'sales');
    await member('bob', 'sales');
    await setPolicy('engineering', ['engineering']);
    for (const u of ['alice', 'dave', 'carol', 'bob']) tok[u] = await h.token(u);
  });
  after(async () => {
    await h.close();
    await up.close();
  });

  async function setPolicy(group: string, sharing: string[]) {
    const r = await h.call('PUT', `/api/v1/admin/policy/groups/${group}/draft`, admin, { document: { knowledge: { sharing: { groups: sharing } } }, priority: 10, reason: 'test' });
    assert.equal(r.status, 200, JSON.stringify(r.body));
    assert.equal((await h.call('POST', `/api/v1/admin/policy/groups/${group}/publish`, admin, { reason: 'test' })).status, 200);
  }
  const publish = (u: string, id: string, text: string, groups: string[], extra: Record<string, unknown> = {}) =>
    h.call('PUT', `/api/v1/knowledge/items/${id}`, tok[u]!, { title: `Doc ${id}`, kind: 'files', text, content_hash: hash(text), groups, ...extra });
  const search = async (u: string, query: string) => (await h.call('POST', '/api/v1/knowledge/search', tok[u]!, { query })).body.passages as any[];

  const ROADMAP = 'The braking system roadmap for 2027 moves the regenerative brake controller to the new platform.\n\nTesting starts in March.';

  it('policy keys: knowledge.sharing.groups and knowledge.sources.notes are in the schema and validated', async () => {
    const schema = (await h.call('GET', '/api/v1/admin/policy/schema?keys=true', admin)).body;
    const keys = JSON.stringify(schema);
    assert.match(keys, /knowledge\.sharing\.groups/);
    assert.match(keys, /knowledge\.sources\.notes/);
    const bad = await h.call('PUT', '/api/v1/admin/policy/groups/sales/draft', admin, { document: { knowledge: { sharing: { groups: 'sales' }, sources: { notes: 'maybe' } } }, priority: 10, reason: 'test' });
    assert.equal(bad.status, 400);
    assert.match(bad.body.error.message, /policy\.knowledge\.sharing\.groups/);
    assert.match(bad.body.error.message, /policy\.knowledge\.sources\.notes/);
    // A guardrail restricts the permitted Groups.
    const g = await h.call('PUT', '/api/v1/admin/policy/guardrail/draft', admin, { document: { knowledge: { sharing: { groups: ['engineering', 'sales'] } } }, priority: 0, reason: 'test' });
    assert.equal(g.status, 200, JSON.stringify(g.body));
  });

  it('chunks like the client: about 1 KB windows', () => {
    const chunks = chunkText(Array.from({ length: 40 }, (_, i) => `Paragraph ${i} ${'word '.repeat(20)}`).join('\n\n'));
    assert.ok(chunks.length > 3);
    assert.ok(chunks.every((c) => c.length <= 1024));
  });

  it('publishes to a permitted Group only; refuses other Groups and oversized text', async () => {
    // Not a member of sales.
    const notMember = await publish('alice', 'doc-x', ROADMAP, ['sales']);
    assert.equal(notMember.status, 403);
    assert.equal(notMember.body.error.code, 'GROUP_NOT_PERMITTED');
    // Member of sales, but policy permits no Group.
    assert.equal((await publish('bob', 'doc-b', ROADMAP, ['sales'])).status, 403);
    assert.equal((h.cp.db.prepare('SELECT COUNT(*) AS n FROM team_items').get() as any).n, 0);
    // Too large.
    const big = await publish('alice', 'doc-big', 'x'.repeat(20_001), ['engineering']);
    assert.equal(big.status, 413);
    assert.equal(big.body.error.code, 'TOO_LARGE');

    const ok = await publish('alice', 'doc-1', ROADMAP, ['engineering']);
    assert.equal(ok.status, 200, JSON.stringify(ok.body));
    assert.equal(ok.body.owner, 'alice');
    assert.deepEqual(ok.body.groups, ['engineering']);
    assert.equal(ok.body.embedded, true);
    // Embedded on the owner's behalf, in their usage.
    assert.ok((h.cp.db.prepare("SELECT COUNT(*) AS n FROM llm_usage WHERE user_name = 'alice' AND endpoint = 'embeddings' AND outcome = 'ok'").get() as any).n >= 1);
    const mine = (await h.call('GET', '/api/v1/knowledge/items', tok.alice!)).body;
    assert.deepEqual(mine.permitted_groups, ['engineering']);
    assert.deepEqual(mine.items.map((i: any) => i.item_id), ['doc-1']);
    const audit = h.cp.audit.query({ action: 'knowledge.publish' }) as any[];
    assert.equal(audit[0].user_name, 'alice');
    assert.ok(!JSON.stringify(audit).includes('regenerative'));
  });

  it('search returns items of the caller\'s Groups only, with provenance', async () => {
    const found = await search('dave', 'regenerative brake roadmap');
    assert.equal(found[0].item_id, 'doc-1');
    assert.equal(found[0].owner, 'alice');
    assert.deepEqual(found[0].groups, ['engineering']);
    assert.equal(found[0].kind, 'files');
    assert.match(found[0].text, /regenerative/);
    assert.deepEqual(await search('carol', 'regenerative brake roadmap'), []);
  });

  it('re-publishing replaces text and Groups; quota per owner', async () => {
    const text = 'Updated: the steering column supplier changes in 2027.';
    assert.equal((await publish('alice', 'doc-1', text, ['engineering'])).status, 200);
    assert.ok(!(await search('dave', 'regenerative')).some((p) => /regenerative/.test(p.text)));
    assert.equal((await search('dave', 'steering supplier'))[0].item_id, 'doc-1');
    const quota = await publish('alice', 'doc-q', 'y '.repeat(9_000), ['engineering']);
    assert.equal(quota.status, 200);
    const over = await publish('alice', 'doc-q2', 'z '.repeat(9_000), ['engineering']);
    assert.equal(over.status, 413);
    assert.equal(over.body.error.code, 'QUOTA_EXCEEDED');
    assert.equal((await h.call('DELETE', '/api/v1/knowledge/items/doc-q', tok.alice!)).status, 200);
  });

  it('unshare deletes text, chunks and vectors', async () => {
    assert.equal((await publish('alice', 'doc-2', 'Gearbox calibration notes for the plant in Springfield.', ['engineering'])).status, 200);
    assert.equal((await search('dave', 'gearbox calibration'))[0].item_id, 'doc-2');
    assert.equal((await h.call('DELETE', '/api/v1/knowledge/items/doc-2', tok.alice!)).status, 200);
    assert.ok(!(await search('dave', 'gearbox calibration')).some((p) => p.item_id === 'doc-2'));
    assert.equal((h.cp.db.prepare("SELECT COUNT(*) AS n FROM team_items WHERE item_id = 'doc-2'").get() as any).n, 0);
    const fts = h.cp.db.prepare("SELECT COUNT(*) AS n FROM team_chunks_fts WHERE team_chunks_fts MATCH 'gearbox'").get() as any;
    assert.equal(fts.n, 0);
    assert.equal((await h.call('DELETE', '/api/v1/knowledge/items/doc-2', tok.alice!)).status, 404);
    assert.equal(h.cp.audit.query({ action: 'knowledge.unshare' }).length, 2);
  });

  it('a Group no longer permitted stops returning the owner\'s items after their next request', async () => {
    await setPolicy('engineering', []);
    await h.call('GET', '/api/v1/knowledge/items', tok.alice!);
    assert.deepEqual(await search('dave', 'steering supplier'), []);
    await setPolicy('engineering', ['*']);
    assert.deepEqual((await h.call('GET', '/api/v1/knowledge/items', tok.alice!)).body.permitted_groups, ['engineering']);
    assert.equal((await search('dave', 'steering supplier'))[0].item_id, 'doc-1');
  });

  it('a revoked owner\'s items are not returned', async () => {
    assert.equal((await publish('dave', 'dave-1', 'Paint shop schedule for the weekend shift.', ['engineering'])).status, 200);
    assert.equal((await search('alice', 'paint shop schedule'))[0].owner, 'dave');
    const r = await h.call('POST', '/api/v1/admin/identity/deny-list', admin, { kind: 'user', value: 'dave', reason: 'left the organization' });
    assert.ok(r.status < 300, JSON.stringify(r.body));
    assert.ok(!(await search('alice', 'paint shop schedule')).some((p) => p.owner === 'dave'));
  });

  it('platform-admins list by Group and remove with a reason; the owner sees it is no longer shared', async () => {
    assert.equal((await h.call('GET', '/api/v1/admin/knowledge/items', tok.alice!)).status, 403);
    const list = (await h.call('GET', '/api/v1/admin/knowledge/items?group=engineering', admin)).body;
    assert.ok(list.groups.some((g: any) => g.group === 'engineering' && g.items >= 1));
    const item = list.items.find((i: any) => i.item_id === 'doc-1');
    assert.equal(item.owner, 'alice');
    assert.ok(item.size > 0);
    assert.equal((await h.call('DELETE', `/api/v1/admin/knowledge/items/${item.id}`, admin, {})).status, 400);
    assert.equal((await h.call('DELETE', `/api/v1/admin/knowledge/items/${item.id}`, admin, { reason: 'contains customer data' })).status, 200);
    const rec = (h.cp.audit.query({ action: 'knowledge.remove' }) as any[])[0];
    assert.equal(rec.user_name, 'admin1');
    assert.equal(rec.reason, 'contains customer data');
    assert.equal(rec.details.owner, 'alice');
    assert.equal(rec.details.title, 'Doc doc-1');
    assert.deepEqual((await h.call('GET', '/api/v1/knowledge/items', tok.alice!)).body.items, []);
  });
});
