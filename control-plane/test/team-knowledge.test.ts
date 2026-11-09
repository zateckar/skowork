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
      c.identity.clockSkewS = 0;
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

  it('advertises publication support honestly without changing ordinary item sharing', async () => {
    const capabilities = await h.call('GET', '/api/v1/knowledge/capabilities', tok.alice!);
    assert.equal(capabilities.status, 200);
    assert.deepEqual(capabilities.body, { publication_read_versions: [], publication_mutation_versions: [] });
    assert.equal((await h.call('GET', '/api/v1/knowledge/capabilities', null)).status, 401);
    const item = 'discovery-compatibility';
    assert.equal((await publish('dave', item, 'Ordinary document remains available.', ['engineering'])).status, 200);
    const unsupported = await h.call('PUT', '/api/v1/knowledge/publications/unsupported', tok.dave!, { schema_version: 1 });
    assert.equal(unsupported.status, 404);
    assert.ok((await search('alice', 'Ordinary document')).some((p) => p.title === `Doc ${item}`));
    assert.equal((await h.call('DELETE', `/api/v1/knowledge/items/${item}`, tok.dave!)).status, 200);
  });

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

  it('checks uploaded text independently of the client hash, including legacy rows', async () => {
    const id = 'digest-check';
    const clientHash = 'legacy-client-hash';
    const original = 'Original spacecraft handbook.';
    const updated = 'Updated spacecraft launch schedule.';
    assert.equal((await publish('dave', id, original, ['engineering'], { content_hash: clientHash })).status, 200);
    assert.equal((await publish('dave', id, updated, ['engineering'], { content_hash: clientHash })).status, 200);
    const row = h.cp.db.prepare('SELECT id, content_hash, server_content_hash FROM team_items WHERE owner = ? AND item_id = ?')
      .get('dave', id) as { id: number; content_hash: string; server_content_hash: string };
    assert.equal(row.content_hash, clientHash);
    assert.equal(row.server_content_hash, hash(updated));
    const chunks = () => h.cp.db.prepare('SELECT text FROM team_chunks WHERE item = ? ORDER BY seq').all(row.id) as { text: string }[];
    assert.deepEqual(chunks().map((chunk) => chunk.text), [updated]);

    // A migrated item has no trusted server digest. Its first later upload must rebuild its passages.
    h.cp.db.prepare("UPDATE team_items SET server_content_hash = '' WHERE id = ?").run(row.id);
    h.cp.db.prepare('UPDATE team_chunks SET text = ? WHERE item = ?').run('Old legacy passage.', row.id);
    assert.equal((await publish('dave', id, updated, ['engineering'], { content_hash: clientHash })).status, 200);
    assert.deepEqual(chunks().map((chunk) => chunk.text), [updated]);
    assert.equal((await h.call('DELETE', `/api/v1/knowledge/items/${id}`, tok.dave!)).status, 200);
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

  it('rechecks withdrawn, newly forbidden and revoked items after an in-flight embedding', async () => {
    assert.equal((await h.call('POST', '/api/v1/admin/identity/group-members', admin,
      { user_name: 'race-owner', workbench_group: 'engineering' })).status, 200);
    assert.equal((await h.call('POST', '/api/v1/admin/identity/group-members', admin,
      { user_name: 'race-reader', workbench_group: 'engineering' })).status, 200);
    tok['race-owner'] = await h.token('race-owner');
    tok['race-reader'] = await h.token('race-reader');
    for (const action of ['withdraw', 'reader', 'expiry', 'reader-revoke', 'policy', 'revoke'] as const) {
      const id = `race-${action}`;
      const text = `Unique ${action} knowledge passage for a delayed search.`;
      assert.equal((await publish('race-owner', id, text, ['engineering'])).status, 200);
      const original = up.handler;
      let signalStarted!: () => void;
      let release!: () => void;
      const started = new Promise<void>((resolve) => { signalStarted = resolve; });
      const held = new Promise<void>((resolve) => { release = resolve; });
      up.handler = (path, body, res) => {
        const inputs = Array.isArray(body.input) ? body.input : [body.input];
        if (path.endsWith('/embeddings') && inputs.includes(text)) {
          signalStarted();
          void held.then(() => original(path, body, res));
        } else original(path, body, res);
      };
      try {
        const reader = action === 'reader-revoke' ? tok['race-reader']! : action === 'expiry'
          ? await h.token('dave', {}, { lifetimeS: 2 }) : tok.dave!;
        const pending = h.call('POST', '/api/v1/knowledge/search', reader, { query: text });
        await Promise.race([started, new Promise<never>((_, reject) => setTimeout(() => reject(new Error('embedding did not start')), 5000))]);
        if (action === 'policy') await setPolicy('engineering', []);
        if (action === 'reader') assert.equal((await h.call('DELETE', '/api/v1/admin/identity/group-members', admin,
          { user_name: 'dave', workbench_group: 'engineering' })).status, 200);
        if (action === 'reader-revoke') assert.equal((await h.call('POST', '/api/v1/admin/identity/deny-list', admin,
          { kind: 'user', value: 'race-reader', reason: 'test revocation' })).status, 200);
        if (action === 'expiry') await new Promise((resolve) => setTimeout(resolve, 2100));
        const changed = action === 'withdraw' ? await h.call('DELETE', `/api/v1/knowledge/items/${id}`, tok['race-owner']!)
          : action === 'policy' ? await h.call('GET', '/api/v1/knowledge/items', tok['race-owner']!)
          : action === 'revoke' ? await h.call('POST', '/api/v1/admin/identity/deny-list', admin, { kind: 'user', value: 'race-owner', reason: 'test revocation' })
          : { status: 200, body: {} };
        assert.ok(changed.status < 300, JSON.stringify(changed.body));
        release();
        const response = await pending;
        assert.equal(response.status, action === 'expiry' ? 401 : action === 'reader-revoke' ? 403 : 200, JSON.stringify(response.body));
        if (response.status === 200) assert.ok(!response.body.passages.some((passage: { item_id: string }) => passage.item_id === id));
      } finally {
        release();
        up.handler = original;
        if (action === 'policy') await setPolicy('engineering', ['engineering']);
        if (action === 'reader') await h.call('POST', '/api/v1/admin/identity/group-members', admin,
          { user_name: 'dave', workbench_group: 'engineering' });
      }
    }
  });

  it('rechecks existing updates after asynchronous embedding', async () => {
    assert.equal((await h.call('POST', '/api/v1/admin/identity/group-members', admin,
      { user_name: 'write-owner', workbench_group: 'engineering' })).status, 200);
    assert.equal((await h.call('POST', '/api/v1/admin/identity/group-members', admin,
      { user_name: 'write-owner', workbench_group: 'quality' })).status, 200);
    await setPolicy('quality', ['quality']);
    tok['write-owner'] = await h.token('write-owner');
    for (const action of ['withdraw', 'recreate', 'concurrent', 'audience', 'membership', 'membership-direct', 'policy', 'quota', 'expiry', 'revoke'] as const) {
      const id = `update-${action}`;
      assert.equal((await publish('write-owner', id, `Original ${action} text.`, ['engineering'])).status, 200);
      const text = action === 'quota' ? 'A '.repeat(9000) : `Changed ${action} text.`;
      const original = up.handler;
      let signalStarted!: () => void;
      let release!: () => void;
      const started = new Promise<void>((resolve) => { signalStarted = resolve; });
      const held = new Promise<void>((resolve) => { release = resolve; });
      up.handler = (path, body, res) => {
        const inputs: string[] = Array.isArray(body.input) ? body.input : [body.input];
        if (path.endsWith('/embeddings') && inputs.some((input) => text.includes(input) && input.length > 0)) {
          signalStarted();
          void held.then(() => original(path, body, res));
        } else original(path, body, res);
      };
      try {
        if (action === 'expiry') tok['write-owner'] = await h.token('write-owner', {}, { lifetimeS: 2 });
        const pending = publish('write-owner', id, text, ['engineering']);
        await Promise.race([started, new Promise<never>((_, reject) => setTimeout(() => reject(new Error('embedding did not start')), 5000))]);
        if (action === 'withdraw' || action === 'recreate') assert.equal((await h.call('DELETE', `/api/v1/knowledge/items/${id}`, tok['write-owner']!)).status, 200);
        if (action === 'recreate' || action === 'concurrent') assert.equal((await publish('write-owner', id, 'Replacement text.', ['engineering'])).status, 200);
        if (action === 'audience') assert.equal((await publish('write-owner', id, 'Original audience text.', ['quality'])).status, 200);
        if (action === 'membership' || action === 'membership-direct') {
          assert.equal((await h.call('DELETE', '/api/v1/admin/identity/group-members', admin,
            { user_name: 'write-owner', workbench_group: 'engineering' })).status, 200);
          if (action === 'membership') assert.equal((await h.call('GET', '/api/v1/knowledge/items', tok['write-owner']!)).status, 200);
        }
        if (action === 'policy') await setPolicy('engineering', []);
        if (action === 'quota') assert.equal((await publish('write-owner', 'quota-other', 'B '.repeat(9000), ['engineering'])).status, 200);
        if (action === 'revoke') assert.equal((await h.call('POST', '/api/v1/admin/identity/deny-list', admin,
          { kind: 'user', value: 'write-owner', reason: 'test revocation' })).status, 200);
        if (action === 'expiry') await new Promise((resolve) => setTimeout(resolve, 2100));
        release();
        const response = await pending;
        assert.equal(response.status, ['withdraw', 'recreate', 'concurrent', 'audience', 'membership'].includes(action) ? 409 : action === 'quota' ? 413 : action === 'expiry' ? 401 : 403, JSON.stringify(response.body));
        const row = h.cp.db.prepare('SELECT content_hash FROM team_items WHERE owner = ? AND item_id = ?').get('write-owner', id) as { content_hash: string } | undefined;
        if (action === 'withdraw') assert.equal(row, undefined);
        else assert.equal(row?.content_hash, hash(action === 'recreate' || action === 'concurrent' ? 'Replacement text.' : `Original ${action} text.`));
        if (action === 'audience') {
          const result = await h.call('GET', '/api/v1/knowledge/items', tok['write-owner']!);
          assert.deepEqual(result.body.items.find((item: { item_id: string }) => item.item_id === id)?.groups, ['quality']);
        }
        if (action === 'membership' || action === 'membership-direct') {
          const snapshot = h.cp.db.prepare('SELECT groups FROM team_owners WHERE user_name = ?').get('write-owner') as { groups: string };
          assert.ok(!(JSON.parse(snapshot.groups) as string[]).includes('engineering'));
        }
      } finally {
        release();
        up.handler = original;
        if (action === 'policy') await setPolicy('engineering', ['engineering']);
        if (action === 'membership' || action === 'membership-direct') await h.call('POST', '/api/v1/admin/identity/group-members', admin,
          { user_name: 'write-owner', workbench_group: 'engineering' });
        if (action === 'quota') await h.call('DELETE', '/api/v1/knowledge/items/quota-other', tok['write-owner']!);
        if (action === 'expiry') tok['write-owner'] = await h.token('write-owner');
      }
    }
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
    assert.equal((h.cp.audit.query({ action: 'knowledge.unshare' }) as { user_name: string }[]).filter((row) => row.user_name === 'alice').length, 2);
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
