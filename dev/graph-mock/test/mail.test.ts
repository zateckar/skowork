// dev/graph-mock mail endpoints, captured sending, delta and fixtures (tasks 2.2, 2.3).
import { test, after, before } from 'node:test';
import assert from 'node:assert/strict';
import type { GraphMock } from '../src/mock.ts';
import { LABEL_PROPERTY } from '../src/state.ts';
import { startMock, as, control, all } from './helpers.ts';

let m: GraphMock;
let g: Awaited<ReturnType<typeof as>>;
before(async () => { m = await startMock(); g = await as(m, 'jan'); });
after(async () => { await m.close(); });

const LABEL_EXPAND = `$expand=singleValueExtendedProperties($filter=id eq '${LABEL_PROPERTY}')`;

test('two mocks with the same seed return identical data; another seed changes the ids', async () => {
  const other = await startMock();
  const third = await startMock({ seed: 'other' });
  try {
    const g2 = await as(other, 'jan');
    const g3 = await as(third, 'jan');
    const strip = (v: any[]) => v.map(({ '@odata.etag': _e, ...rest }) => rest);
    const a = await all(g, '/me/messages?$top=50');
    const b = await all(g2, '/me/messages?$top=50');
    assert.deepEqual(strip(a.items), strip(b.items));
    const ev = '/me/calendarView?startDateTime=2026-10-01T00:00:00Z&endDateTime=2026-12-01T00:00:00Z&$top=200';
    assert.deepEqual((await all(g, ev)).items, (await all(g2, ev)).items);
    const c = await all(g3, '/me/messages?$top=50');
    assert.equal(c.items.length, a.items.length);
    assert.notEqual(c.items[0].id, a.items[0].id);
  } finally {
    await other.close();
    await third.close();
  }
});

test('fixtures carry cs/de/en content, internal and external senders, labels and attachments', async () => {
  const msgs = (await all(g, `/me/messages?$top=100&${LABEL_EXPAND}`)).items;
  const subjects = msgs.map((x) => x.subject);
  assert.ok(subjects.includes('Cenová nabídka – servis linky 4'));
  assert.ok(subjects.includes('Budget Q4 – Entwurf'));
  assert.ok(subjects.includes('Project Atlas – status'));
  assert.ok(msgs.some((x) => x.from.emailAddress.address.endsWith('@dodavatel-test.example')));
  const board = msgs.find((x) => x.subject.startsWith('Board pack'));
  assert.match(board.singleValueExtendedProperties[0].value, /MSIP_Label_0f1e7b2a-1c39-4a0d-9d41-7a3f00000004_Enabled=true/);
  assert.equal(board.singleValueExtendedProperties[0].id, LABEL_PROPERTY);
  const unlabelled = msgs.find((x) => x.subject.startsWith('Webinar'));
  assert.deepEqual(unlabelled.singleValueExtendedProperties, []);
  // Without the $expand there are no extended properties.
  assert.equal((await g.get(`/me/messages/${board.id}`)).json.singleValueExtendedProperties, undefined);
});

test('list with $filter, $select, $top, $orderby and paging links', async () => {
  const f = await g.get(`/me/mailFolders/inbox/messages?$filter=${encodeURIComponent("receivedDateTime ge 2026-10-22T00:00:00Z and from/emailAddress/address eq 'tom.baker@acme-test.example'")}&$select=subject,from,receivedDateTime&$orderby=receivedDateTime desc`);
  assert.equal(f.status, 200, f.text);
  assert.deepEqual(f.json.value.map((x: any) => x.subject), ['Board pack – strictly confidential', 'RE: Budget Q4 – Entwurf']);
  assert.deepEqual(Object.keys(f.json.value[0]).sort(), ['@odata.etag', 'from', 'id', 'receivedDateTime', 'subject']);
  const p1 = await g.get('/me/messages?$top=3');
  assert.equal(p1.json.value.length, 3);
  assert.ok(p1.json['@odata.nextLink']);
  const rest = await all(g, p1.json['@odata.nextLink']);
  assert.equal(rest.items.length + 3, (await all(g, '/me/messages?$top=100')).items.length);
  const hasAtt = await g.get(`/me/messages?$filter=${encodeURIComponent('hasAttachments eq true')}`);
  assert.equal(hasAtt.json.value.length, 3);
  const contains = await g.get(`/me/messages?$filter=${encodeURIComponent("contains(subject,'atlas')")}`);
  assert.equal(contains.json.value.length, 1);
  const cats = await g.get(`/me/messages?$filter=${encodeURIComponent("categories/any(c:c eq 'HR')")}`);
  assert.equal(cats.json.value[0].subject, 'Hodnocení zaměstnanců 2026');
  const bad = await g.get(`/me/messages?$filter=${encodeURIComponent('subject has x')}`);
  assert.equal(bad.status, 400);
  assert.equal(bad.json.error.code, 'BadRequest');
});

test('$search is full text over subject, body, sender and attachment names', async () => {
  const s = await g.get('/me/messages?$search="linky 4"');
  assert.deepEqual(s.json.value.map((x: any) => x.subject), ['Cenová nabídka – servis linky 4']);
  assert.equal((await g.get('/me/messages?$search="Budget-Q4.docx"')).json.value.length, 1);
  assert.equal((await g.get('/me/messages?$search="x"&$orderby=subject')).status, 400);
});

test('text bodies on request, attachments as base64 and raw', async () => {
  const offer = (await g.get(`/me/messages?$search="Petra"`)).json.value[0];
  const t = await g.get(`/me/messages/${offer.id}`, { prefer: 'outlook.body-content-type="text"' });
  assert.equal(t.json.body.contentType, 'text');
  assert.match(t.json.body.content, /^Dobrý den, pane Nováku,\n/);
  const atts = await g.get(`/me/messages/${offer.id}/attachments?$select=id,name,contentType,size`);
  assert.equal(atts.json.value[0].name, 'Nabidka-servis-linka-4.pdf');
  assert.equal(atts.json.value[0].contentBytes, undefined);
  const one = await g.get(`/me/messages/${offer.id}/attachments/${atts.json.value[0].id}`);
  assert.ok(Buffer.from(one.json.contentBytes, 'base64').toString('latin1').startsWith('%PDF-1.4'));
  const raw = await g.get(`/me/messages/${offer.id}/attachments/${atts.json.value[0].id}/$value`);
  assert.equal(raw.status, 200);
  assert.match(raw.headers.get('content-type') ?? '', /^application\/pdf/);
  assert.ok(raw.text.startsWith('%PDF-1.4'));
  const budget = (await g.get(`/me/messages?$search="Budget-Q4.docx"`)).json.value[0];
  const docx = (await g.get(`/me/messages/${budget.id}/attachments`)).json.value[0];
  assert.equal(Buffer.from(docx.contentBytes, 'base64').subarray(0, 2).toString('latin1'), 'PK');
});

test('drafts, replies and captured sending', async () => {
  const c = control(m);
  await c.del('/sends');
  const thread = (await g.get(`/me/messages?$search="licence line"`)).json.value[0];
  const reply = await g.post(`/me/messages/${thread.id}/createReply`, { comment: 'Čtvrtek mi vyhovuje.' });
  assert.equal(reply.status, 201);
  assert.equal(reply.json.isDraft, true);
  assert.equal(reply.json.subject, 'RE: Budget Q4 – Entwurf');
  assert.equal(reply.json.conversationId, thread.conversationId);
  const drafts = (await g.get('/me/mailFolders/drafts/messages')).json.value;
  assert.ok(drafts.some((d: any) => d.id === reply.json.id));
  assert.equal((await c.get('/sends')).json.length, 0, 'a draft sends nothing');

  const patched = await g.patch(`/me/messages/${reply.json.id}`, { body: { contentType: 'text', content: 'Čtvrtek 13:00 mi vyhovuje.' } });
  assert.equal(patched.status, 200);
  assert.equal((await g.post(`/me/messages/${reply.json.id}/send`)).status, 202);
  const sends = (await c.get('/sends')).json;
  assert.equal(sends.length, 1);
  assert.deepEqual(sends[0].to, ['tom.baker@acme-test.example']);
  assert.equal(sends[0].body, 'Čtvrtek 13:00 mi vyhovuje.');
  // Sent Items for the sender, the inbox of the internal recipient.
  assert.ok((await g.get('/me/mailFolders/sentitems/messages')).json.value.some((x: any) => x.subject === 'RE: Budget Q4 – Entwurf' && !x.isDraft));
  const tom = await as(m, 'tom');
  assert.ok((await tom.get('/me/mailFolders/inbox/messages')).json.value.some((x: any) => x.subject === 'RE: Budget Q4 – Entwurf'));

  // sendMail to an external address: captured and in Sent Items, nobody else receives it.
  const ext = await g.post('/me/sendMail', { message: { subject: 'Objednávka', body: { contentType: 'text', content: 'Objednáváme servis.' }, toRecipients: [{ emailAddress: { address: 'petra.mala@dodavatel-test.example' } }], attachments: [{ '@odata.type': '#microsoft.graph.fileAttachment', name: 'po.txt', contentBytes: Buffer.from('PO 1').toString('base64') }] } });
  assert.equal(ext.status, 202);
  const all2 = (await c.get('/sends?kind=mail')).json;
  assert.deepEqual(all2[1].to, ['petra.mala@dodavatel-test.example']);
  assert.deepEqual(all2[1].attachments, ['po.txt']);
});

test('move gives a new id in the new folder; delete removes', async () => {
  const n = (await g.get(`/me/messages?$search="Webinar"`)).json.value[0];
  const moved = await g.post(`/me/messages/${n.id}/move`, { destinationId: 'deleteditems' });
  assert.equal(moved.status, 201);
  assert.notEqual(moved.json.id, n.id);
  assert.equal(moved.json.parentFolderId, (await g.get('/me/mailFolders/deleteditems')).json.id);
  assert.equal((await g.get(`/me/messages/${n.id}`)).status, 404);
  assert.equal((await g.del(`/me/messages/${moved.json.id}`)).status, 204);
  assert.equal((await g.get(`/me/messages/${moved.json.id}`)).status, 404);
});

test('delta: pages, then only changes; removals; delivered mail; expiry', async () => {
  const c = control(m);
  const first = await all(g, '/me/mailFolders/inbox/messages/delta?$select=subject,receivedDateTime', { prefer: 'odata.maxpagesize=3' });
  assert.equal(first.status, 200);
  assert.ok(first.pages > 1);
  assert.ok(first.deltaLink);
  const inboxCount = (await all(g, '/me/mailFolders/inbox/messages?$top=100')).items.length;
  assert.equal(first.items.length, inboxCount);

  const quiet = await all(g, first.deltaLink!);
  assert.equal(quiet.items.length, 0);

  const delivered = await c.post('/users/jan/messages', { from: 'eva', subject: 'Neue Nachricht', body: '<p>Hallo</p>' });
  const some = (await g.get(`/me/messages?$search="Hodnocení"`)).json.value[0];
  await g.patch(`/me/messages/${some.id}`, { isRead: true });
  const moved = (await g.get(`/me/messages?$search="Board pack"`)).json.value[0];
  await g.post(`/me/messages/${moved.id}/move`, { destinationId: 'archive' });
  const next = await all(g, quiet.deltaLink!);
  assert.deepEqual(next.items.filter((x) => !x['@removed']).map((x) => x.subject).sort(), ['Hodnocení zaměstnanců 2026', 'Neue Nachricht']);
  assert.deepEqual(next.items.filter((x) => x['@removed']).map((x) => x.id), [moved.id]);
  assert.ok(next.items.some((x) => x.id === delivered.json.id));

  // An expired token answers 410; a fresh full sync works.
  await c.post('/faults', { kind: 'delta_expired', path: '/messages/delta', count: 1 });
  const expired = await g.get(next.deltaLink!);
  assert.equal(expired.status, 410);
  assert.equal(expired.json.error.code, 'syncStateNotFound');
  await c.post('/delta/expire');
  assert.equal((await g.get(next.deltaLink!)).status, 410);
  assert.equal((await all(g, '/me/mailFolders/inbox/messages/delta')).status, 200);
});

test('an endpoint the mock does not implement answers with a Graph-shaped error', async () => {
  const r = await g.get('/me/contacts');
  assert.equal(r.status, 501);
  assert.equal(r.json.error.code, 'NotMocked');
  assert.match(r.json.error.message, /GET \/v1.0\/me\/contacts/);
});
