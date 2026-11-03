// dev/graph-mock calendar endpoints (task 2.4).
import { test, after, before } from 'node:test';
import assert from 'node:assert/strict';
import type { GraphMock } from '../src/mock.ts';
import { startMock, as, control, all } from './helpers.ts';

let m: GraphMock;
let g: Awaited<ReturnType<typeof as>>;
before(async () => { m = await startMock(); g = await as(m, 'jan'); });
after(async () => { await m.close(); });

const WEEK = 'startDateTime=2026-10-26T00:00:00Z&endDateTime=2026-11-02T00:00:00Z';

test('calendarView expands the recurring stand-up and keeps single events', async () => {
  const v = (await all(g, `/me/calendarView?${WEEK}&$top=50`)).items;
  const standups = v.filter((e) => e.subject === 'Daily stand-up');
  assert.equal(standups.length, 5);
  assert.ok(standups.every((e) => e.type === 'occurrence' && e.seriesMasterId));
  assert.equal(standups[0].start.dateTime, '2026-10-26T07:30:00.0000000');
  assert.equal(standups[0].start.timeZone, 'UTC');
  assert.ok(v.some((e) => e.subject === 'Budget Q4 review' && e.type === 'singleInstance' && e.isOrganizer === false));
  // The master and an occurrence can be read by id.
  assert.equal((await g.get(`/me/events/${standups[0].seriesMasterId}`)).json.type, 'seriesMaster');
  assert.equal((await g.get(`/me/events/${standups[2].id}`)).json.start.dateTime, '2026-10-28T07:30:00.0000000');
  assert.equal((await g.get(`/me/calendarView?startDateTime=x`)).status, 400);
});

test('private items carry their sensitivity', async () => {
  const v = (await all(g, `/me/calendarView?${WEEK}&$top=50`)).items;
  const doctor = v.find((e) => e.subject === 'Lékař');
  assert.equal(doctor.sensitivity, 'private');
  assert.equal(doctor.showAs, 'oof');
});

test('getSchedule returns free/busy of fixture users and an error for unknown mailboxes', async () => {
  const r = await g.post('/me/calendar/getSchedule', {
    schedules: ['jan.novak@acme-test.example', 'eva.schneider@acme-test.example', 'tom.baker@acme-test.example', 'nobody@example.com'],
    startTime: { dateTime: '2026-10-27T08:00:00', timeZone: 'UTC' }, endTime: { dateTime: '2026-10-27T14:00:00', timeZone: 'UTC' }, availabilityViewInterval: 60,
  });
  assert.equal(r.status, 200);
  const [jan, eva, tom, nobody] = r.json.value;
  assert.equal(jan.availabilityView, '000033'); // stand-up is at 07:30; the doctor (oof) 12:00-13:30
  assert.equal(eva.availabilityView, '222000');
  assert.equal(tom.availabilityView, '004444');
  assert.equal(nobody.error.responseCode, 'ErrorMailRecipientNotFound');
  assert.equal(jan.scheduleItems.find((i: any) => i.isPrivate).subject, undefined);
});

test('create sends invitations (captured) and puts a copy into internal attendees calendars; transactionId is idempotent', async () => {
  const c = control(m);
  await c.del('/sends');
  const body = {
    subject: 'Atlas kalibrace', start: { dateTime: '2026-10-29T09:00:00', timeZone: 'UTC' }, end: { dateTime: '2026-10-29T09:30:00', timeZone: 'UTC' },
    attendees: [{ emailAddress: { address: 'tom.baker@acme-test.example', name: 'Tom Baker' }, type: 'required' }, { emailAddress: { address: 'ext@partner-test.example' }, type: 'optional' }],
    transactionId: 'tx-1',
  };
  const e = await g.post('/me/events', body);
  assert.equal(e.status, 201);
  assert.equal(e.json.isOrganizer, true);
  const again = await g.post('/me/events', body);
  assert.equal(again.json.id, e.json.id);
  const sends = (await c.get('/sends?kind=invitation')).json;
  assert.equal(sends.length, 1);
  assert.deepEqual(sends[0].to, ['tom.baker@acme-test.example', 'ext@partner-test.example']);
  const tom = await as(m, 'tom');
  const copy = (await all(tom, '/me/calendarView?startDateTime=2026-10-29T00:00:00Z&endDateTime=2026-10-30T00:00:00Z')).items.find((x) => x.subject === 'Atlas kalibrace');
  assert.equal(copy.isOrganizer, false);
  assert.equal(copy.responseStatus.response, 'notResponded');

  // Tom accepts; Jan's copy shows it, and the response is captured.
  assert.equal((await tom.post(`/me/events/${copy.id}/accept`, { comment: 'OK', sendResponse: true })).status, 202);
  const mine = (await g.get(`/me/events/${e.json.id}`)).json;
  assert.equal(mine.attendees.find((a: any) => a.emailAddress.address === 'tom.baker@acme-test.example').status.response, 'accepted');
  assert.equal((await c.get('/sends?kind=response')).json[0].response, 'accepted');
  assert.equal((await g.post(`/me/events/${e.json.id}/decline`, {})).status, 400, 'the organizer cannot respond');
});

test('delta reports a reschedule once, cancellations and removals', async () => {
  const c = control(m);
  const first = await all(g, `/me/calendarView/delta?${WEEK}`, { prefer: 'odata.maxpagesize=4' });
  assert.equal(first.status, 200);
  assert.ok(first.pages > 1);
  assert.equal((await all(g, first.deltaLink!)).items.length, 0);

  // Eva (the organiser) moves the budget review: Jan's copy changes once.
  const evaCopy = (await all(await as(m, 'eva'), `/me/calendarView?${WEEK}`)).items.find((x) => x.subject === 'Budget Q4 review');
  await c.patch(`/users/eva/events/${evaCopy.id}`, { start: '+3d 15:00' });
  const d1 = await all(g, first.deltaLink!);
  assert.equal(d1.items.length, 1);
  assert.equal(d1.items[0].subject, 'Budget Q4 review');
  assert.equal(d1.items[0].start.dateTime, '2026-10-29T15:00:00.0000000');
  assert.equal((await all(g, d1.deltaLink!)).items.length, 0);

  // Tom cancels the board meeting: Jan's copy is cancelled.
  const tomBoard = (await all(await as(m, 'tom'), `/me/calendarView?${WEEK}`)).items.find((x) => x.subject === 'Board – plant 3');
  await c.post(`/users/tom/events/${tomBoard.id}/cancel`);
  const d2 = await all(g, d1.deltaLink!);
  assert.equal(d2.items.length, 1);
  assert.equal(d2.items[0].isCancelled, true);
  assert.match(d2.items[0].subject, /^Canceled: /);

  // Jan deletes the recurring stand-up he organises: every occurrence in the window is removed.
  const master = d2.items.length ? (await g.get(`/me/events/${first.items.find((x) => x.subject === 'Daily stand-up').seriesMasterId}`)).json : null;
  assert.equal((await g.del(`/me/events/${master.id}`)).status, 204);
  const d3 = await all(g, d2.deltaLink!);
  assert.equal(d3.items.filter((x) => x['@removed']).length, 5);
});

test('events are created only with a valid time range', async () => {
  const r = await g.post('/me/events', { subject: 'x', start: { dateTime: '2026-10-29T10:00:00', timeZone: 'UTC' }, end: { dateTime: '2026-10-29T09:00:00', timeZone: 'UTC' } });
  assert.equal(r.status, 400);
});

test('online meetings carry their join link; a room meeting has none', async () => {
  const v = (await all(g, `/me/calendarView?${WEEK}&$top=50`)).items;
  // (The online stand-up was deleted by an earlier test; the supplier call is online too.)
  const supplier = v.find((e) => e.subject === 'Servis linky 4 – jednání');
  assert.equal(supplier.isOnlineMeeting, true);
  assert.match(supplier.onlineMeeting.joinUrl, /^https:\/\/teams\.microsoft\.com\/l\/meetup-join\//);
  const review = v.find((e) => e.subject === 'Budget Q4 review');
  assert.equal(review.isOnlineMeeting, false);
  assert.equal(review.onlineMeeting, null);
  const created = await g.post('/me/events', { subject: 'Ad-hoc sync', isOnlineMeeting: true,
    start: { dateTime: '2026-10-29T10:00:00', timeZone: 'UTC' }, end: { dateTime: '2026-10-29T10:30:00', timeZone: 'UTC' } });
  assert.equal(created.status, 201);
  assert.ok(created.json.onlineMeeting.joinUrl);
});