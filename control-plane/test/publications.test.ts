// control-plane/team-knowledge, improve-skowork-and-knowledge-control tasks 9.1-9.5: versioned publications reached
// through the real server — capability negotiation, version header, atomic revisions, current-authority reads,
// withdrawal barriers, administrator removal, Company Group, metadata-only audit and older-client search.
import { after, before, describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { fakeUpstream, json, sleep, startHarness, type FakeUpstream, type Harness } from './helpers.ts';

function embed(text: string): number[] {
  const v = new Array(64).fill(0);
  for (const w of text.toLowerCase().match(/[\p{L}\p{N}]+/gu) ?? []) v[createHash('sha1').update(w).digest()[0]! % 64] += 1;
  return v;
}

const V1 = { 'skowork-publication-version': '1' };
const ROADMAP = 'The regenerative brake controller moves to the new platform in March 2027.';
const BUDGET = 'The prototype test budget for the brake controller is approved.';

describe('team knowledge publications', () => {
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
      // Inputs marked SLOW answer late, so another request can commit while this one waits for its embedding.
      if (inputs.some((t) => t.includes('SLOW'))) {
        setTimeout(() => json(res, 200, { data: inputs.map((t, index) => ({ index, embedding: embed(t) })), usage: { prompt_tokens: 1, total_tokens: 1 } }), 400);
        return;
      }
      json(res, 200, { data: inputs.map((t, index) => ({ index, embedding: embed(t) })), usage: { prompt_tokens: 1, total_tokens: 1 } });
    };
    h = await startHarness((c) => {
      c.gateway.upstreams = [{ name: 'a', baseUrl: up.url, apiKey: 'k-0123456789', maxConcurrent: 0 }];
      c.identity.clockSkewS = 0;
      c.teamKnowledge.maxItemBytes = 20_000;
      c.teamKnowledge.quotaBytes = 30_000;
    });
    admin = await h.token('admin1');
    for (const [u, g] of [['alice', 'engineering'], ['dave', 'engineering'], ['carol', 'sales'], ['quinn', 'qa']]) await member(u!, g!);
    await setPolicy('engineering', { groups: ['engineering'] });
    await setPolicy('qa', { groups: ['qa'] });
    for (const u of ['alice', 'dave', 'carol', 'quinn']) tok[u] = await h.token(u);
  });
  after(async () => {
    await h.close();
    await up.close();
  });

  const member = async (u: string, g: string) =>
    assert.equal((await h.call('POST', '/api/v1/admin/identity/group-members', admin, { user_name: u, skowork_group: g })).status, 200);
  const unmember = async (u: string, g: string) =>
    assert.equal((await h.call('DELETE', '/api/v1/admin/identity/group-members', admin, { user_name: u, skowork_group: g })).status, 200);
  async function setPolicy(group: string, sharing: Record<string, unknown>) {
    const r = await h.call('PUT', `/api/v1/admin/policy/groups/${group}/draft`, admin, { document: { knowledge: { sharing } }, priority: 10, reason: 'test' });
    assert.equal(r.status, 200, JSON.stringify(r.body));
    assert.equal((await h.call('POST', `/api/v1/admin/policy/groups/${group}/publish`, admin, { reason: 'test' })).status, 200);
  }
  let op = 0;
  const envelope = (expected: number, groups: string[], entries: { id: string; text: string; kinds?: string[]; evidence?: string }[], title = 'Brake program') => ({
    expected_revision: expected, operation_id: `op-${++op}`, title, groups,
    entries: entries.map((e) => ({ id: e.id, text: e.text, source_kinds: e.kinds ?? ['files'],
      evidence: e.evidence ? [{ id: `${e.id}-x`, title: 'Plan.docx', text: e.evidence }] : [] })),
  });
  const put = (token: string, id: string, body: unknown, headers: Record<string, string> = V1) => h.call('PUT', `/api/v1/knowledge/publications/${id}`, token, body, headers);
  const search = async (u: string, query: string) => {
    const r = await h.call('POST', '/api/v1/knowledge/publications/search', tok[u]!, { query });
    assert.equal(r.status, 200, JSON.stringify(r.body));
    return r.body.passages as any[];
  };
  const mine = async (token: string) => (await h.call('GET', '/api/v1/knowledge/publications', token)).body;
  const keyOf = async (token: string, id: string) => ((await mine(token)).publications as any[]).find((p) => p.publication_id === id)!.publication as string;
  const read = (u: string, key: string) => h.call('GET', `/api/v1/knowledge/publications/shared/${key}`, tok[u]!);

  it('advertises publications only while the routes are active', async () => {
    h.cfg.teamKnowledge.publications = false;
    assert.deepEqual((await h.call('GET', '/api/v1/knowledge/capabilities', tok.alice!)).body, { publication_read_versions: [], publication_mutation_versions: [] });
    assert.equal((await put(tok.dave!, 'p0', envelope(0, ['engineering'], [{ id: 'e1', text: ROADMAP }]))).status, 404);
    assert.equal((await h.call('GET', '/api/v1/knowledge/publications', tok.dave!)).status, 404);
    h.cfg.teamKnowledge.publications = true;
    assert.deepEqual((await h.call('GET', '/api/v1/knowledge/capabilities', tok.alice!)).body, { publication_read_versions: [1], publication_mutation_versions: [1] });
    for (const [method, path] of [['GET', '/publications'], ['PUT', '/publications/p0'], ['DELETE', '/publications/p0'], ['DELETE', '/publications/p0/entries/e1'],
      ['POST', '/publications/search'], ['GET', '/publications/shared/x'], ['GET', '/capabilities']] as const)
      assert.equal((await h.call(method, `/api/v1/knowledge${path}`, null, method === 'GET' ? undefined : {})).status, 401, `${method} ${path}`);
    for (const [method, path] of [['GET', '/publications'], ['GET', '/publications/x'], ['DELETE', '/publications/x'], ['POST', '/publications/x/resolve']] as const)
      assert.equal((await h.call(method, `/api/v1/admin/knowledge${path}`, tok.alice!, method === 'GET' ? undefined : { reason: 'r' })).status, 403, `admin ${method} ${path}`);
  });

  it('refuses a mutation without an advertised version and changes nothing', async () => {
    assert.equal((await put(tok.dave!, 'p-version', envelope(0, ['engineering'], [{ id: 'e1', text: ROADMAP }]), {})).body.error.code, 'UNSUPPORTED_VERSION');
    assert.equal((await put(tok.dave!, 'p-version', envelope(0, ['engineering'], [{ id: 'e1', text: ROADMAP }]), { 'skowork-publication-version': '2' })).status, 400);
    assert.equal((await h.call('DELETE', '/api/v1/knowledge/publications/p-version', tok.dave!, { expected_revision: 0, operation_id: 'x' })).status, 400);
    assert.equal(((await mine(tok.dave!)).publications as any[]).some((p) => p.publication_id === 'p-version'), false);
  });

  it('publishes exactly the confirmed entries, fences stale revisions and replays retries', async () => {
    const first = envelope(0, ['engineering'], [{ id: 'date', text: ROADMAP, evidence: 'Platform switch: March 2027.' }, { id: 'budget', text: BUDGET }]);
    const r = await put(tok.dave!, 'brakes', first);
    assert.equal(r.status, 200, JSON.stringify(r.body));
    assert.equal(r.body.revision, 1);
    assert.equal((await put(tok.dave!, 'brakes', first)).body.replayed, true);
    // A second device that still holds revision 0 is refused and revision 1 keeps its audience.
    const stale = await put(tok.dave!, 'brakes', envelope(0, ['engineering'], [{ id: 'other', text: 'Stale overwrite.' }]));
    assert.equal(stale.status, 409);
    const key = await keyOf(tok.dave!, 'brakes');
    const doc = await read('alice', key);
    assert.equal(doc.status, 200);
    assert.deepEqual(doc.body.entries.map((e: any) => e.entry_id), ['date', 'budget']);
    assert.equal(doc.body.revision, 1);
    const page = await h.call('GET', `/api/v1/knowledge/publications/shared/${key}?offset=1&limit=1`, tok.alice!);
    assert.deepEqual([page.body.entries.map((e: any) => e.entry_id), page.body.total_entries, page.body.next_offset], [['budget'], 2, null]);
    assert.equal((await h.call('GET', `/api/v1/knowledge/publications/shared/${key}?limit=1`, tok.alice!)).body.next_offset, 1);
    assert.equal(doc.body.owner, 'dave');
    assert.deepEqual(doc.body.entries[0].evidence, [{ id: 'date-x', title: 'Plan.docx', text: 'Platform switch: March 2027.' }]);
    const hits = await search('alice', 'regenerative brake controller platform');
    const hit = hits.find((p) => p.entry_id === 'date');
    assert.ok(hit, JSON.stringify(hits));
    assert.equal(hit.owner, 'dave');
    assert.equal(hit.revision, 1);
    assert.deepEqual(hit.groups, ['engineering']);
    assert.equal(hit.evidence[0].title, 'Plan.docx');
    assert.ok(hit.updated_at);
    // Other Groups see nothing: no passage, title or count, and a direct read looks like a missing publication.
    assert.deepEqual(await search('carol', 'regenerative brake controller'), []);
    assert.equal((await read('carol', key)).status, 404);
    // Publishing to a Group the owner is not in is refused.
    assert.equal((await put(tok.dave!, 'sales-only', envelope(0, ['sales'], [{ id: 'e', text: ROADMAP }]))).status, 403);
  });

  it('refuses a Source kind that policy does not permit and keeps the previous revision', async () => {
    const r = await put(tok.dave!, 'brakes', envelope(1, ['engineering'], [{ id: 'mail', text: 'From the inbox.', kinds: ['mail'] }]));
    assert.equal(r.status, 403);
    assert.equal(r.body.error.code, 'SOURCE_NOT_PERMITTED');
    assert.equal(((await mine(tok.dave!)).publications as any[]).find((p) => p.publication_id === 'brakes').revision, 1);
    // Policy that permits mail lets it through; narrowing policy later hides that entry without an owner upload.
    await setPolicy('engineering', { groups: ['engineering'], derived_sources: ['files', 'notes', 'mail'] });
    assert.equal((await put(tok.dave!, 'mailed', envelope(0, ['engineering'], [{ id: 'm', text: 'Inbox note about the brake supplier.', kinds: ['mail'] },
      { id: 'f', text: 'File note about the brake supplier.' }]))).status, 200);
    const key = await keyOf(tok.dave!, 'mailed');
    assert.equal((await read('alice', key)).body.entries.length, 2);
    await setPolicy('engineering', { groups: ['engineering'] });
    assert.deepEqual((await read('alice', key)).body.entries.map((e: any) => e.entry_id), ['f']);
    assert.ok((await search('alice', 'brake supplier')).every((p) => p.entry_id !== 'm'));
  });

  it('re-evaluates reader membership, owner policy and owner revocation on every read', async () => {
    const key = await keyOf(tok.dave!, 'brakes');
    await unmember('alice', 'engineering');
    assert.deepEqual(await search('alice', 'regenerative brake controller'), []);
    assert.equal((await read('alice', key)).status, 404);
    await member('alice', 'engineering');
    assert.equal((await read('alice', key)).status, 200);

    await setPolicy('engineering', { groups: [] });
    assert.equal((await read('alice', key)).status, 404);
    assert.deepEqual(await search('alice', 'regenerative brake controller'), []);
    await setPolicy('engineering', { groups: ['engineering'] });
    assert.equal((await read('alice', key)).status, 200);

    const denied = await h.call('POST', '/api/v1/admin/identity/deny-list', admin, { kind: 'user', value: 'dave', reason: 'test' });
    assert.equal(denied.status, 200);
    assert.equal((await read('alice', key)).status, 404);
    const entry = ((await h.call('GET', '/api/v1/admin/identity/deny-list', admin)).body as any[]).find((d) => d.value === 'dave');
    assert.equal((await h.call('DELETE', `/api/v1/admin/identity/deny-list/${entry.id}`, admin)).status, 200);
    assert.equal((await read('alice', key)).status, 200);
  });

  it('stops counting Groups from identity-provider roles once their token has expired', async () => {
    const mapped = await h.call('PUT', '/api/v1/admin/identity/role-mappings', admin, { mappings: [{ idp_role: 'role-qa', skowork_group: 'qa' }] });
    assert.equal(mapped.status, 200);
    const frank = await h.token('frank', { roles: ['role-qa'] }, { lifetimeS: 3 });
    const r = await put(frank, 'qa-notes', envelope(0, ['qa'], [{ id: 'q', text: 'Quality gate for the brake controller passes.' }]));
    assert.equal(r.status, 200, JSON.stringify(r.body));
    const key = await keyOf(frank, 'qa-notes');
    assert.equal((await read('quinn', key)).status, 200);
    await sleep(3200);
    assert.equal((await read('quinn', key)).status, 404);
    assert.deepEqual((await search('quinn', 'quality gate')).filter((p) => p.publication === key), []);
  });

  it('withdraws one entry, then the last, keeping the revision barrier', async () => {
    const key = await keyOf(tok.dave!, 'brakes');
    const del = (path: string, expected: number, headers = V1) =>
      h.call('DELETE', `/api/v1/knowledge/publications/brakes${path}`, tok.dave!, { expected_revision: expected, operation_id: `w-${++op}` }, headers);
    assert.equal((await del('/entries/budget', 1)).body.revision, 2);
    assert.deepEqual((await read('alice', key)).body.entries.map((e: any) => e.entry_id), ['date']);
    assert.ok((await search('alice', 'prototype test budget approved')).every((p) => p.entry_id !== 'budget'));
    const leftovers = h.cp.db.prepare("SELECT (SELECT count(*) FROM knowledge_publication_chunks WHERE entry_id = 'budget') + (SELECT count(*) FROM knowledge_publication_evidence WHERE entry_id = 'budget') AS n").get() as { n: number };
    assert.equal(Number(leftovers.n), 0);
    // A delayed upload of the original revision cannot restore the withdrawn entry.
    assert.equal((await put(tok.dave!, 'brakes', envelope(1, ['engineering'], [{ id: 'budget', text: BUDGET }]))).status, 409);
    assert.equal((await del('/entries/missing', 2)).status, 404);
    assert.equal((await del('/entries/date', 2)).body.revision, 3);
    assert.equal((await read('alice', key)).status, 404);
    const own = ((await mine(tok.dave!)).publications as any[]).find((p) => p.publication_id === 'brakes');
    assert.equal(own.state, 'withdrawn');
    assert.equal(own.revision, 3);
    assert.equal(own.title, null);
    // Republishing is a fresh confirmation against the barrier's revision.
    assert.equal((await put(tok.dave!, 'brakes', envelope(3, ['engineering'], [{ id: 'date', text: ROADMAP }]))).body.revision, 4);
  });

  it('lets the same issuer and subject manage a publication after a username change', async () => {
    const renamed = await h.token('dave-renamed', { sub: 'sub-dave' });
    const own = ((await mine(renamed)).publications as any[]).find((p) => p.publication_id === 'brakes');
    assert.equal(own.revision, 4);
    // Someone else who later uses the former username is a different subject and sees none of it.
    const impostor = await h.token('dave', { sub: 'sub-someone-else' });
    assert.deepEqual((await mine(impostor)).publications, []);
    assert.equal((await h.call('DELETE', '/api/v1/knowledge/publications/brakes', impostor, { expected_revision: 4, operation_id: 'steal' }, V1)).status, 409);
    // Back to the owner's usual name, so later tests keep their Group memberships.
    assert.equal((await mine(tok.dave!)).publications.length > 0, true);
  });

  it('lets an administrator inspect, remove with a reason and resolve; audit carries no knowledge text', async () => {
    const key = await keyOf(tok.dave!, 'brakes');
    const list = (await h.call('GET', '/api/v1/admin/knowledge/publications', admin)).body.publications as any[];
    assert.ok(list.some((p) => p.publication === key && p.owner === 'dave' && p.state === 'published'));
    const exact = await h.call('GET', `/api/v1/admin/knowledge/publications/${key}`, admin);
    assert.equal(exact.body.revision, 4);
    assert.deepEqual(exact.body.groups, ['engineering']);
    assert.equal(exact.body.entries[0].text, ROADMAP);
    assert.equal((await h.call('DELETE', `/api/v1/admin/knowledge/publications/${key}`, admin, {})).status, 400);
    assert.equal((await h.call('DELETE', `/api/v1/admin/knowledge/publications/${key}`, admin, { reason: 'Contains a supplier price.' })).status, 200);
    assert.ok((await search('alice', 'regenerative brake controller')).every((p) => p.publication !== key));
    const own = ((await mine(tok.dave!)).publications as any[]).find((p) => p.publication_id === 'brakes');
    assert.equal(own.state, 'removed');
    assert.equal(own.removal.reason, 'Contains a supplier price.');
    // A late retry and a fresh publish both stay refused until an administrator resolves the removal.
    assert.equal((await put(tok.dave!, 'brakes', envelope(4, ['engineering'], [{ id: 'date', text: ROADMAP }]))).status, 409);
    assert.equal((await put(tok.dave!, 'brakes', envelope(5, ['engineering'], [{ id: 'date', text: ROADMAP }]))).status, 409);
    assert.equal((await h.call('POST', `/api/v1/admin/knowledge/publications/${key}/resolve`, admin, {})).status, 200);
    assert.equal((await put(tok.dave!, 'brakes', envelope(6, ['engineering'], [{ id: 'date', text: ROADMAP }]))).body.revision, 7);
    // Withdrawing a whole publication removes all of it at once.
    const mailed = await keyOf(tok.dave!, 'mailed');
    assert.equal((await h.call('DELETE', '/api/v1/knowledge/publications/mailed', tok.dave!, { expected_revision: 1, operation_id: 'withdraw-all' }, V1)).body.revision, 2);
    assert.equal((await read('alice', mailed)).status, 404);
    assert.equal(Number((h.cp.db.prepare("SELECT count(*) AS n FROM knowledge_publication_entries WHERE publication_id = 'mailed'").get() as { n: number }).n), 0);
    const audit = h.cp.db.prepare("SELECT action, details FROM audit WHERE action LIKE 'knowledge.publication.%'").all() as { action: string; details: string }[];
    for (const a of ['knowledge.publication.publish', 'knowledge.publication.withdraw', 'knowledge.publication.withdraw-entry', 'knowledge.publication.admin-remove', 'knowledge.publication.admin-resolve'])
      assert.ok(audit.some((r) => r.action === a), a);
    const all = JSON.stringify(audit);
    for (const secret of [ROADMAP, BUDGET, 'Platform switch', 'Brake program']) assert.equal(all.includes(secret), false, secret);
  });

  it('offers a configured Company Group only when membership and policy permit it, and never expands * to everyone', async () => {
    assert.equal((await mine(tok.dave!)).company_group, null);
    h.cfg.teamKnowledge.companyGroup = 'company';
    assert.equal((await mine(tok.dave!)).company_group, null);
    assert.equal((await put(tok.dave!, 'all-hands', envelope(0, ['company'], [{ id: 'a', text: ROADMAP }]))).status, 403);
    await member('dave', 'company');
    await setPolicy('company', { groups: ['*'] });
    const view = await mine(tok.dave!);
    assert.equal(view.company_group, 'company');
    assert.equal(view.permitted_groups.includes('everyone'), false);
    assert.equal((await put(tok.dave!, 'all-hands', envelope(0, ['everyone'], [{ id: 'a', text: ROADMAP }]))).status, 403);
    assert.equal((await put(tok.dave!, 'all-hands', envelope(0, ['company'], [{ id: 'a', text: ROADMAP }]))).status, 200);
    h.cfg.teamKnowledge.companyGroup = null;
  });

  it('enforces the owner quota shared with ordinary items, again after embedding', async () => {
    const big = 'x'.repeat(9_000);
    const entries = [1, 2].map((i) => ({ id: `big${i}`, text: `${big} ${i}` }));
    // The slow publication fits when it starts; the quota is used up while it waits for its embedding.
    const slow = put(tok.dave!, 'slow', envelope(0, ['engineering'], [{ id: 's', text: `SLOW ${big}${'y'.repeat(3_000)}` }]));
    await sleep(100);
    assert.equal((await put(tok.dave!, 'big-a', envelope(0, ['engineering'], entries))).status, 200);
    assert.equal((await slow).status, 413);
    assert.equal(h.cp.db.prepare(`SELECT 1 FROM knowledge_publication_ledger WHERE publication_id = 'slow'`).get(), undefined);
    assert.equal((await put(tok.dave!, 'big-b', envelope(0, ['engineering'], entries))).status, 413);
  });

  it('gives older clients publication passages through item search, with citations', async () => {
    const r = await h.call('POST', '/api/v1/knowledge/search', tok.alice!, { query: 'regenerative brake controller platform' });
    assert.equal(r.status, 200);
    const hit = (r.body.passages as any[]).find((p) => p.publication);
    assert.ok(hit, JSON.stringify(r.body));
    assert.match(hit.item_id, /^publication:/);
    assert.equal(hit.owner, 'dave');
    assert.equal(typeof hit.publication.revision, 'number');
    // An ordinary item write cannot touch the publication.
    assert.equal((await h.call('DELETE', `/api/v1/knowledge/items/${encodeURIComponent(hit.item_id)}`, tok.dave!)).status, 404);
  });
});
