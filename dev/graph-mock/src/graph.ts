// The Graph v1.0 surface (dev/graph-mock Graph Subset Compatibility): /me, mailbox settings, mail folders, messages
// with the OData subset, attachments, the msip_labels extended property, message delta, drafts and send, events,
// calendar view with recurrence expansion, event delta, getSchedule and event responses. Only the mock's own tokens
// are accepted (Mock Authentication), and each call needs the delegated scope Graph would require.
import { Router, type Request, type Response, type NextFunction } from 'express';
import { jwtVerify, importJWK } from 'jose';
import { randomUUID } from 'node:crypto';
import type { GraphMock } from './mock.ts';
import { iso, LABEL_PROPERTY, teamsJoinUrl, type Address, type CalEvent, type Message, type User, type Attendee } from './state.ts';
import { applyOrderBy, applySelect, evaluate, matchesSearch, ODataError, parseFilter } from './odata.ts';

type Req = Request & { user?: User; scopes?: string[] };

export function graphError(res: Response, status: number, code: string, message: string): void {
  const date = new Date().toISOString();
  res.status(status).json({ error: { code, message, innerError: { date, 'request-id': randomUUID(), 'client-request-id': randomUUID() } } });
}

// ---- HTML and text ---------------------------------------------------------------------------------------------------

export function htmlToText(html: string): string {
  return html
    .replace(/<\s*(br|\/p|\/div|\/li|\/tr|\/h\d)\s*\/?>/gi, '\n')
    .replace(/<[^>]+>/g, '')
    .replace(/&nbsp;/g, ' ')
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&#39;/g, "'")
    .replace(/&amp;/g, '&')
    .replace(/\n{3,}/g, '\n\n')
    .trim();
}
const textToHtml = (t: string) => '<html><body>' + t.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/\n/g, '<br>') + '</body></html>';

function wantsText(req: Request): boolean {
  return /outlook\.body-content-type\s*=\s*"?text"?/i.test(req.get('prefer') ?? '');
}
function maxPageSize(req: Request, fallback: number): number {
  const m = /odata\.maxpagesize\s*=\s*(\d+)/i.exec(req.get('prefer') ?? '');
  return m ? Math.max(1, Math.min(1000, Number(m[1]))) : fallback;
}

// ---- serialisation -----------------------------------------------------------------------------------------------------

const recip = (a: Address) => ({ emailAddress: { name: a.name, address: a.address } });

function labelValue(mock: GraphMock, label: string | null): string | null {
  if (!label) return null;
  return `MSIP_Label_${label}_Enabled=true; MSIP_Label_${label}_SetDate=2026-01-05T09:00:00Z; MSIP_Label_${label}_Method=Standard; MSIP_Label_${label}_SiteId=${mock.state.seed}-tenant`;
}

/** How items are rendered: text or html bodies, the label property, $select. */
export interface View { text: boolean; label: boolean; select?: string }
export function viewOf(req: Request, select: string | undefined = req.query.$select as string | undefined): View {
  const e = String(req.query.$expand ?? '');
  return { text: wantsText(req), label: /singleValueExtendedProperties/i.test(e) && e.includes('00020386-0000-0000-C000-000000000046'), select };
}

export function messageJson(mock: GraphMock, m: Message, v: View): Record<string, unknown> {
  const text = v.text;
  const body = text ? htmlToText(m.bodyHtml) : m.bodyHtml;
  const o: Record<string, unknown> = {
    '@odata.etag': `W/"${Buffer.from(String(m.seq)).toString('base64')}"`,
    id: m.id,
    createdDateTime: m.createdDateTime,
    lastModifiedDateTime: m.lastModifiedDateTime,
    changeKey: Buffer.from(String(m.seq)).toString('base64'),
    categories: m.categories,
    receivedDateTime: m.receivedDateTime,
    sentDateTime: m.isDraft ? null : m.sentDateTime,
    hasAttachments: m.attachments.length > 0,
    internetMessageId: m.internetMessageId,
    subject: m.subject,
    bodyPreview: htmlToText(m.bodyHtml).slice(0, 255),
    importance: m.importance,
    parentFolderId: m.folderId,
    conversationId: m.conversationId,
    isRead: m.isRead,
    isDraft: m.isDraft,
    webLink: `https://outlook.office365.com/owa/?ItemID=${encodeURIComponent(m.id)}&exvsurl=1&viewmodel=ReadMessageItem`,
    body: { contentType: text ? 'text' : 'html', content: body },
    sender: recip(m.from),
    from: recip(m.from),
    toRecipients: m.to.map(recip),
    ccRecipients: m.cc.map(recip),
    bccRecipients: [],
    replyTo: [],
    flag: { flagStatus: 'notFlagged' },
  };
  if (v.label) {
    const v = labelValue(mock, m.label);
    o.singleValueExtendedProperties = v ? [{ id: LABEL_PROPERTY, value: v }] : [];
  }
  return applySelect(o, v.select);
}

const dt = (s: string) => ({ dateTime: s.replace(/Z$/, '.0000000'), timeZone: 'UTC' });

export function eventJson(mock: GraphMock, e: CalEvent, v: View, occ?: { id: string; start: string; end: string }): Record<string, unknown> {
  const text = v.text;
  const o: Record<string, unknown> = {
    '@odata.etag': `W/"${Buffer.from(String(e.seq)).toString('base64')}"`,
    id: occ ? occ.id : e.id,
    createdDateTime: e.createdDateTime,
    lastModifiedDateTime: e.lastModifiedDateTime,
    changeKey: Buffer.from(String(e.seq)).toString('base64'),
    categories: e.categories,
    iCalUId: e.iCalUId,
    subject: e.subject,
    bodyPreview: htmlToText(e.bodyHtml).slice(0, 255),
    importance: 'normal',
    sensitivity: e.sensitivity,
    isAllDay: e.isAllDay,
    isCancelled: e.isCancelled,
    isOrganizer: e.isOrganizer,
    responseRequested: true,
    seriesMasterId: occ ? e.id : null,
    showAs: e.showAs,
    type: occ ? 'occurrence' : e.recurrence ? 'seriesMaster' : 'singleInstance',
    webLink: `https://outlook.office365.com/owa/?itemid=${encodeURIComponent(occ ? occ.id : e.id)}&exvsurl=1&path=/calendar/item`,
    responseStatus: { response: e.response, time: '0001-01-01T00:00:00Z' },
    body: { contentType: text ? 'text' : 'html', content: text ? htmlToText(e.bodyHtml) : e.bodyHtml },
    start: dt(occ ? occ.start : e.start),
    end: dt(occ ? occ.end : e.end),
    location: { displayName: e.location, locationType: 'default' },
    isOnlineMeeting: !!e.onlineMeetingUrl,
    onlineMeetingProvider: e.onlineMeetingUrl ? 'teamsForBusiness' : 'unknown',
    onlineMeeting: e.onlineMeetingUrl ? { joinUrl: e.onlineMeetingUrl } : null,
    locations: e.location ? [{ displayName: e.location }] : [],
    recurrence: occ || !e.recurrence ? null : {
      pattern: { type: e.recurrence.type, interval: e.recurrence.interval, daysOfWeek: e.recurrence.daysOfWeek, firstDayOfWeek: 'monday', month: 0, dayOfMonth: 0, index: 'first' },
      range: { type: e.recurrence.endDate ? 'endDate' : 'noEnd', startDate: e.recurrence.startDate, endDate: e.recurrence.endDate ?? '0001-01-01', recurrenceTimeZone: 'UTC', numberOfOccurrences: 0 },
    },
    attendees: e.attendees.map((a) => ({ type: a.type, status: { response: a.response, time: '0001-01-01T00:00:00Z' }, emailAddress: { name: a.name, address: a.address } })),
    organizer: recip(e.organizer),
  };
  if (v.label) {
    const v = labelValue(mock, e.label);
    o.singleValueExtendedProperties = v ? [{ id: LABEL_PROPERTY, value: v }] : [];
  }
  return applySelect(o, v.select);
}

// ---- recurrence --------------------------------------------------------------------------------------------------------

const DAYS = ['sunday', 'monday', 'tuesday', 'wednesday', 'thursday', 'friday', 'saturday'];

/** The instances of an event overlapping [from, to): the event itself, or the occurrences of a series. */
export function instances(mock: GraphMock, e: CalEvent, from: number, to: number): { id: string; start: string; end: string; occurrence: boolean }[] {
  const s = Date.parse(e.start), len = Date.parse(e.end) - s;
  if (!e.recurrence) return s < to && s + len > from ? [{ id: e.id, start: e.start, end: e.end, occurrence: false }] : [];
  const out: { id: string; start: string; end: string; occurrence: boolean }[] = [];
  const r = e.recurrence;
  const last = r.endDate ? Date.parse(r.endDate + 'T23:59:59Z') : Infinity;
  const startDay = Date.UTC(new Date(s).getUTCFullYear(), new Date(s).getUTCMonth(), new Date(s).getUTCDate());
  const timeOfDay = s - startDay;
  for (let day = startDay, i = 0; day <= Math.min(last, to) && i < 5000; day += 86_400_000, i++) {
    const n = Math.round((day - startDay) / 86_400_000);
    let hit: boolean;
    if (r.type === 'daily') hit = n % r.interval === 0;
    else {
      const week = Math.floor((n + ((new Date(startDay).getUTCDay() + 6) % 7)) / 7);
      hit = week % r.interval === 0 && r.daysOfWeek.includes(DAYS[new Date(day).getUTCDay()]!);
    }
    if (!hit) continue;
    const os = day + timeOfDay;
    if (os < to && os + len > from) {
      const date = new Date(day).toISOString().slice(0, 10);
      out.push({ id: `${e.id}_occ_${date.replace(/-/g, '')}`, start: iso(new Date(os)), end: iso(new Date(os + len)), occurrence: true });
    }
  }
  return out;
}

// ---- the router ----------------------------------------------------------------------------------------------------------

const SCOPES: { re: RegExp; method?: string; any: string[] }[] = [
  { re: /^\/me\/?$/, any: ['User.Read'] },
  // department, jobTitle and manager need User.Read.All (User.ReadBasic.All covers names and addresses only).
  { re: /^\/users\/[^/]+\/manager$/, any: ['User.Read.All'] },
  { re: /^\/users\/[^/]+$/, any: ['User.Read.All', 'User.ReadBasic.All'] },
  { re: /^\/me\/mailboxSettings/, any: ['MailboxSettings.Read', 'MailboxSettings.ReadWrite'] },
  { re: /^\/me\/(sendMail|messages\/[^/]+\/send)$/, any: ['Mail.Send'] },
  { re: /^\/me\/(messages|mailFolders)/, method: 'GET', any: ['Mail.Read', 'Mail.ReadWrite'] },
  { re: /^\/me\/(messages|mailFolders)/, any: ['Mail.ReadWrite'] },
  { re: /^\/me\/calendar\/getSchedule$/, any: ['Calendars.Read', 'Calendars.ReadWrite'] },
  { re: /^\/me\/(events|calendarView|calendars|calendar)/, method: 'GET', any: ['Calendars.Read', 'Calendars.ReadWrite'] },
  { re: /^\/me\/(events|calendarView|calendars|calendar)/, any: ['Calendars.ReadWrite'] },
];

export function graphRouter(mock: GraphMock): Router {
  const r = Router();
  let publicKey: Awaited<ReturnType<typeof importJWK>> | undefined;

  // Request log, faults and authentication.
  r.use(async (req: Req, res: Response, next: NextFunction) => {
    const entry = { surface: 'graph' as const, method: req.method, path: '/v1.0' + req.path, query: { ...req.query } };
    res.on('finish', () => mock.log({ ...entry, status: res.statusCode, user: req.user?.key, fault: res.locals.fault as string | undefined }));
    const auth = req.get('authorization') ?? '';
    const m = /^Bearer (.+)$/i.exec(auth);
    if (!m) return graphError(res, 401, 'InvalidAuthenticationToken', 'Access token is empty.');
    try {
      publicKey ??= await importJWK(mock.jwk, 'RS256');
      const { payload } = await jwtVerify(m[1]!, publicKey, { audience: 'https://graph.microsoft.com' });
      if (typeof payload.iss !== 'string' || !payload.iss.startsWith(mock.graphBase + '/') || payload.mock !== true) throw new Error('issuer');
      const u = mock.state.user(String(payload.upn ?? ''));
      if (!u) throw new Error('user');
      req.user = u;
      req.scopes = String(payload.scp ?? '').split(' ').filter(Boolean);
    } catch (e) {
      const expired = (e as { code?: string }).code === 'ERR_JWT_EXPIRED';
      return graphError(res, 401, 'InvalidAuthenticationToken', expired ? 'Lifetime validation failed, the token is expired.' : 'Access token validation failure. Invalid audience or signature.');
    }
    const rule = SCOPES.find((s) => s.re.test(req.path) && (!s.method || s.method === req.method));
    if (rule && !rule.any.some((s) => req.scopes!.includes(s))) return graphError(res, 403, 'ErrorAccessDenied', `Access is denied. Check credentials and try again. (needs ${rule.any.join(' or ')})`);

    const f = mock.takeFault(req, ['throttle', 'server_error', 'slow', 'malformed']);
    if (f) {
      res.locals.fault = f.kind;
      if (f.kind === 'throttle') {
        res.set('Retry-After', String(f.retry_after ?? 10));
        return graphError(res, f.status ?? 429, 'TooManyRequests', 'Application is over its MailboxConcurrency limit.');
      }
      if (f.kind === 'server_error') return graphError(res, f.status ?? 503, 'ServiceNotAvailable', 'The service is temporarily unavailable.');
      if (f.kind === 'slow') await new Promise((d) => setTimeout(d, f.delay_ms ?? 2000));
      if (f.kind === 'malformed') {
        res.status(200).type('application/json').send('{"@odata.context":"https://graph.microsoft.com/v1.0/$metadata#users","value":[{"id":"AAMk');
        return;
      }
    }
    next();
  });

  const base = (req: Request) => `${mock.graphBase}/v1.0`;
  const odata = (fn: (req: Req, res: Response) => void) => (req: Req, res: Response) => {
    try {
      fn(req, res);
    } catch (e) {
      if (e instanceof ODataError) return graphError(res, 400, 'BadRequest', `Invalid filter clause: ${e.message}`);
      throw e;
    }
  };

  /** One page of a list, following Graph's $top/$skip paging. */
  function page(req: Request, res: Response, items: Record<string, unknown>[], context: string, defaultTop = 10): void {
    const top = Math.max(1, Math.min(1000, Number(req.query.$top ?? defaultTop) || defaultTop));
    const skip = Math.max(0, Number(req.query.$skip ?? 0) || 0);
    const slice = items.slice(skip, skip + top);
    const body: Record<string, unknown> = { '@odata.context': `${base(req)}/$metadata#${context}`, value: slice };
    if (skip + top < items.length) {
      const u = new URL(base(req) + req.path);
      for (const [k, v] of Object.entries(req.query)) if (k !== '$skip') u.searchParams.set(k, String(v));
      u.searchParams.set('$top', String(top));
      u.searchParams.set('$skip', String(skip + top));
      body['@odata.nextLink'] = u.toString();
    }
    res.json(body);
  }

  // ---- user and settings ----
  r.get('/me', (req: Req, res) => {
    const u = req.user!;
    res.json(applySelect({ '@odata.context': `${base(req)}/$metadata#users/$entity`, id: u.id, displayName: u.displayName, mail: u.upn, userPrincipalName: u.upn, preferredLanguage: u.language, businessPhones: [], givenName: u.displayName.split(' ')[0], surname: u.displayName.split(' ').slice(1).join(' '), jobTitle: null, officeLocation: null }, req.query.$select as string | undefined));
  });
  // ---- the user directory (add-knowledge-graph design D5) ----
  const directoryUser = (u: User, req: Req) => {
    const all = req.scopes!.includes('User.Read.All');
    const full: Record<string, unknown> = { id: u.id, displayName: u.displayName, mail: u.upn, userPrincipalName: u.upn, givenName: u.displayName.split(' ')[0], surname: u.displayName.split(' ').slice(1).join(' ') };
    if (all) Object.assign(full, { department: u.department, jobTitle: u.jobTitle, officeLocation: null });
    return full;
  };
  r.get('/users/:id', (req: Req, res) => {
    const u = mock.state.user(decodeURIComponent(String(req.params.id)));
    if (!u) return graphError(res, 404, 'Request_ResourceNotFound', `Resource '${req.params.id}' does not exist or one of its queried reference-property objects are not present.`);
    res.json(applySelect({ '@odata.context': `${base(req)}/$metadata#users/$entity`, ...directoryUser(u, req) }, req.query.$select as string | undefined));
  });
  r.get('/users/:id/manager', (req: Req, res) => {
    const u = mock.state.user(decodeURIComponent(String(req.params.id)));
    if (!u) return graphError(res, 404, 'Request_ResourceNotFound', `Resource '${req.params.id}' does not exist or one of its queried reference-property objects are not present.`);
    const m = u.manager ? mock.state.user(u.manager) : undefined;
    if (!m) return graphError(res, 404, 'Request_ResourceNotFound', `Resource 'manager' does not exist or one of its queried reference-property objects are not present.`);
    res.json(applySelect({ '@odata.context': `${base(req)}/$metadata#directoryObjects/$entity`, '@odata.type': '#microsoft.graph.user', ...directoryUser(m, req) }, req.query.$select as string | undefined));
  });

  r.get('/me/mailboxSettings', (req: Req, res) => {
    const u = req.user!;
    res.json({
      '@odata.context': `${base(req)}/$metadata#users('${u.id}')/mailboxSettings`, timeZone: u.timeZone, dateFormat: 'dd.MM.yyyy', timeFormat: 'HH:mm',
      language: { locale: u.language, displayName: u.language }, workingHours: { daysOfWeek: ['monday', 'tuesday', 'wednesday', 'thursday', 'friday'], startTime: '08:00:00.0000000', endTime: '17:00:00.0000000', timeZone: { name: u.timeZone } },
      automaticRepliesSetting: { status: 'disabled' },
    });
  });

  // ---- mail folders ----
  const folderJson = (mock2: GraphMock, f: { id: string; displayName: string; parentFolderId: string; user: string; wellKnownName: string | null }) => {
    const msgs = mock2.state.messages.filter((m) => m.folderId === f.id);
    return { id: f.id, displayName: f.displayName, parentFolderId: f.parentFolderId, childFolderCount: 0, unreadItemCount: msgs.filter((m) => !m.isRead).length, totalItemCount: msgs.length, sizeInBytes: 0, isHidden: false, wellKnownName: f.wellKnownName };
  };
  r.get('/me/mailFolders', odata((req: Req, res) => {
    let items = mock.state.folders.filter((f) => f.user === req.user!.key).map((f) => folderJson(mock, f) as Record<string, unknown>);
    if (req.query.$filter) { const n = parseFilter(String(req.query.$filter)); items = items.filter((i) => evaluate(n, i)); }
    page(req, res, items.map((i) => applySelect(i, req.query.$select as string | undefined)), 'users/mailFolders', 10);
  }));
  r.get('/me/mailFolders/:folder', (req: Req, res) => {
    const f = mock.state.folder(req.user!.key, String(req.params.folder));
    if (!f) return graphError(res, 404, 'ErrorItemNotFound', 'The specified object was not found in the store.');
    res.json(applySelect(folderJson(mock, f), req.query.$select as string | undefined));
  });

  // ---- messages ----
  function listMessages(req: Req, res: Response, folderId: string | null): void {
    const user = req.user!.key;
    let msgs = mock.state.messages.filter((m) => m.user === user && (folderId ? m.folderId === folderId : true));
    const search = req.query.$search as string | undefined;
    if (search && req.query.$orderby) return graphError(res, 400, 'ErrorInvalidUrlQuery', 'The query parameter $orderby is invalid with $search.');
    msgs = msgs.filter((m) => matchesSearch([m.subject, htmlToText(m.bodyHtml), m.from.name, m.from.address, ...m.to.map((t) => t.address), ...m.attachments.map((a) => a.name)], search));
    let items = msgs.map((m) => ({ m, j: messageJson(mock, m, viewOf(req, undefined)) }));
    if (req.query.$filter) { const n = parseFilter(String(req.query.$filter)); items = items.filter((i) => evaluate(n, i.j)); }
    items = applyOrderBy(items.map((i) => ({ ...i.j, __m: i.m })), (req.query.$orderby as string | undefined) ?? 'receivedDateTime desc').map((x) => ({ m: (x as { __m: Message }).__m, j: x }));
    page(req, res, items.map((i) => messageJson(mock, i.m, viewOf(req))), 'users/messages', 10);
  }
  r.get('/me/messages', odata((req: Req, res) => listMessages(req, res, null)));
  r.get('/me/mailFolders/:folder/messages', odata((req: Req, res) => {
    const f = mock.state.folder(req.user!.key, String(req.params.folder));
    if (!f) return graphError(res, 404, 'ErrorItemNotFound', 'The specified folder was not found in the store.');
    listMessages(req, res, f.id);
  }));

  // Message delta (one folder): pages with $skiptoken, then a deltaLink with $deltatoken.
  r.get('/me/mailFolders/:folder/messages/delta', odata((req: Req, res) => {
    const user = req.user!.key;
    const f = mock.state.folder(user, String(req.params.folder));
    if (!f) return graphError(res, 404, 'ErrorItemNotFound', 'The specified folder was not found in the store.');
    const tokenId = (req.query.$deltatoken ?? req.query.$skiptoken) as string | undefined;
    type Cont = { user: string; folder: string; since: number; upto: number; offset: number; generation: number; filter?: string; select?: string; label: boolean };
    let c: Cont;
    if (tokenId) {
      const found = mock.continuations.get(tokenId) as Cont | undefined;
      if (!found || found.user !== user || found.folder !== f.id || found.generation !== mock.state.generation || mock.takeFault(req, ['delta_expired'])) {
        res.locals.fault = 'delta_expired';
        return graphError(res, 410, 'syncStateNotFound', 'The sync state generation is not found. Discard the delta token and start a full sync.');
      }
      c = req.query.$deltatoken ? { ...found, since: found.upto, upto: mock.state.seq, offset: 0 } : found;
    } else {
      c = { user, folder: f.id, since: 0, upto: mock.state.seq, offset: 0, generation: mock.state.generation, filter: req.query.$filter as string | undefined, select: req.query.$select as string | undefined, label: viewOf(req).label };
    }
    // The query options of the first request travel in the token, as Graph's do.
    const q: View = { ...viewOf(req), select: c.select, label: c.label };
    let changed = mock.state.messages.filter((m) => m.user === user && m.folderId === f.id && m.seq > c.since && m.seq <= c.upto);
    if (c.filter && c.since === 0) { const n = parseFilter(c.filter); changed = changed.filter((m) => evaluate(n, messageJson(mock, m, { text: false, label: false }))); }
    changed.sort((a, b) => b.receivedDateTime.localeCompare(a.receivedDateTime)); // newest first
    const removed = c.since === 0 ? [] : mock.state.tombstones.filter((t) => t.kind === 'message' && t.user === user && t.folderId === f.id && t.seq > c.since && t.seq <= c.upto && !changed.some((m) => m.id === t.id));
    const all: Record<string, unknown>[] = [
      ...changed.map((m) => ({ '@odata.type': '#microsoft.graph.message', ...messageJson(mock, m, q) })),
      ...removed.map((t) => ({ '@odata.type': '#microsoft.graph.message', id: t.id, '@removed': { reason: 'deleted' } })),
    ];
    const size = maxPageSize(req, 10);
    const slice = all.slice(c.offset, c.offset + size);
    const body: Record<string, unknown> = { '@odata.context': `${base(req)}/$metadata#Collection(message)`, value: slice };
    const token = mock.nextToken('delta');
    if (c.offset + size < all.length) {
      mock.continuations.set(token, { ...c, offset: c.offset + size });
      body['@odata.nextLink'] = `${base(req)}/me/mailFolders/${encodeURIComponent(String(req.params.folder))}/messages/delta?$skiptoken=${token}`;
    } else {
      mock.continuations.set(token, { ...c, since: c.upto, offset: 0 });
      body['@odata.deltaLink'] = `${base(req)}/me/mailFolders/${encodeURIComponent(String(req.params.folder))}/messages/delta?$deltatoken=${token}`;
    }
    res.json(body);
  }));

  const findMessage = (req: Req, res: Response): Message | undefined => {
    const m = mock.state.messages.find((x) => x.id === String(req.params.id) && x.user === req.user!.key);
    if (!m) graphError(res, 404, 'ErrorItemNotFound', 'The specified object was not found in the store.');
    return m;
  };
  r.get('/me/messages/:id', (req: Req, res) => {
    const m = findMessage(req, res);
    if (m) res.json({ '@odata.context': `${base(req)}/$metadata#users('${req.user!.id}')/messages/$entity`, ...messageJson(mock, m, viewOf(req)) });
  });
  r.get('/me/messages/:id/attachments', (req: Req, res) => {
    const m = findMessage(req, res);
    if (!m) return;
    page(req, res, m.attachments.map((a) => applySelect({ '@odata.type': '#microsoft.graph.fileAttachment', id: a.id, lastModifiedDateTime: a.lastModifiedDateTime, name: a.name, contentType: a.contentType, size: a.bytes.length, isInline: false, contentBytes: a.bytes.toString('base64') }, req.query.$select as string | undefined)), 'attachments', 100);
  });
  r.get('/me/messages/:id/attachments/:aid', (req: Req, res) => {
    const m = findMessage(req, res);
    if (!m) return;
    const a = m.attachments.find((x) => x.id === String(req.params.aid));
    if (!a) return graphError(res, 404, 'ErrorItemNotFound', 'The attachment was not found.');
    res.json({ '@odata.type': '#microsoft.graph.fileAttachment', id: a.id, lastModifiedDateTime: a.lastModifiedDateTime, name: a.name, contentType: a.contentType, size: a.bytes.length, isInline: false, contentBytes: a.bytes.toString('base64') });
  });
  r.get('/me/messages/:id/attachments/:aid/\\$value', (req: Req, res) => {
    const m = findMessage(req, res);
    const a = m?.attachments.find((x) => x.id === String(req.params.aid));
    if (!m) return;
    if (!a) return graphError(res, 404, 'ErrorItemNotFound', 'The attachment was not found.');
    res.type(a.contentType).send(a.bytes);
  });

  const toAddresses = (v: unknown): Address[] =>
    Array.isArray(v) ? v.map((x) => ({ name: String((x as { emailAddress?: { name?: string } }).emailAddress?.name ?? ''), address: String((x as { emailAddress?: { address?: string } }).emailAddress?.address ?? '') })).filter((a) => a.address) : [];
  const bodyOf = (b: unknown): string => {
    const o = (b ?? {}) as { contentType?: string; content?: string };
    return o.contentType?.toLowerCase() === 'text' ? textToHtml(o.content ?? '') : (o.content ?? '');
  };
  const meAddress = (u: User): Address => ({ name: u.displayName, address: u.upn });

  // Drafts.
  r.post('/me/messages', (req: Req, res) => {
    const u = req.user!, b = req.body as Record<string, unknown>, now = mock.now();
    const m = mock.state.addMessage({
      user: u.key, folderId: mock.state.folder(u.key, 'drafts')!.id, subject: String(b.subject ?? ''), bodyHtml: bodyOf(b.body), from: meAddress(u),
      to: toAddresses(b.toRecipients), cc: toAddresses(b.ccRecipients), receivedDateTime: iso(now), sentDateTime: iso(now), isRead: true, isDraft: true,
      importance: String(b.importance ?? 'normal'), conversationId: mock.state.nextId('conversation'), categories: [], label: null, attachments: [],
    }, now);
    res.status(201).json(messageJson(mock, m, viewOf(req)));
  });
  r.patch('/me/messages/:id', (req: Req, res) => {
    const m = findMessage(req, res);
    if (!m) return;
    const b = req.body as Record<string, unknown>;
    if (!m.isDraft && ['subject', 'body', 'toRecipients', 'ccRecipients'].some((k) => k in b)) return graphError(res, 400, 'ErrorInvalidOperation', 'Only drafts can be changed this way.');
    if ('subject' in b) m.subject = String(b.subject ?? '');
    if ('body' in b) m.bodyHtml = bodyOf(b.body);
    if ('toRecipients' in b) m.to = toAddresses(b.toRecipients);
    if ('ccRecipients' in b) m.cc = toAddresses(b.ccRecipients);
    if ('isRead' in b) m.isRead = !!b.isRead;
    if ('categories' in b && Array.isArray(b.categories)) m.categories = b.categories.map(String);
    m.seq = mock.state.bump();
    m.lastModifiedDateTime = iso(mock.now());
    res.json(messageJson(mock, m, viewOf(req)));
  });
  for (const kind of ['createReply', 'createReplyAll', 'createForward'] as const) {
    r.post(`/me/messages/:id/${kind}`, (req: Req, res) => {
      const m = findMessage(req, res);
      if (!m) return;
      const u = req.user!, now = mock.now(), b = (req.body ?? {}) as Record<string, unknown>;
      const prefix = kind === 'createForward' ? 'FW: ' : 'RE: ';
      const to = kind === 'createForward' ? toAddresses(b.toRecipients) : kind === 'createReplyAll' ? [m.from, ...m.to.filter((t) => t.address.toLowerCase() !== u.upn.toLowerCase())] : [m.from];
      const quoted = `<hr><b>From:</b> ${m.from.name} &lt;${m.from.address}&gt;<br><b>Sent:</b> ${m.sentDateTime}<br><b>Subject:</b> ${m.subject}<br><br>${m.bodyHtml}`;
      const comment = typeof b.comment === 'string' ? textToHtml(b.comment) : '';
      const d = mock.state.addMessage({
        user: u.key, folderId: mock.state.folder(u.key, 'drafts')!.id, subject: prefix + m.subject.replace(/^(RE|FW|AW|WG|Odp|Fwd): /i, ''), bodyHtml: comment + quoted, from: meAddress(u),
        to, cc: kind === 'createReplyAll' ? m.cc : [], receivedDateTime: iso(now), sentDateTime: iso(now), isRead: true, isDraft: true, importance: 'normal',
        conversationId: m.conversationId, categories: [], label: null, attachments: kind === 'createForward' ? [...m.attachments] : [],
      }, now);
      res.status(201).json(messageJson(mock, d, viewOf(req)));
    });
  }
  r.post('/me/messages/:id/attachments', (req: Req, res) => {
    const m = findMessage(req, res);
    if (!m) return;
    if (!m.isDraft) return graphError(res, 400, 'ErrorInvalidOperation', 'Attachments can only be added to drafts.');
    const b = req.body as Record<string, unknown>;
    const a = { id: mock.state.nextId('attachment'), name: String(b.name ?? 'attachment'), contentType: String(b.contentType ?? 'application/octet-stream'), bytes: Buffer.from(String(b.contentBytes ?? ''), 'base64'), lastModifiedDateTime: iso(mock.now()) };
    m.attachments.push(a);
    m.seq = mock.state.bump();
    res.status(201).json({ '@odata.type': '#microsoft.graph.fileAttachment', id: a.id, name: a.name, contentType: a.contentType, size: a.bytes.length, isInline: false });
  });

  /** Captured sending (Captured Sending): Sent Items for the sender, the inbox of internal recipients, nothing else. */
  function deliver(u: User, msg: { subject: string; bodyHtml: string; to: Address[]; cc: Address[]; attachments: Message['attachments']; conversationId: string }, saveToSent: boolean): void {
    const now = mock.now();
    mock.state.captures.push({ kind: 'mail', at: iso(now), from: u.upn, to: msg.to.map((a) => a.address), cc: msg.cc.map((a) => a.address), subject: msg.subject, body: htmlToText(msg.bodyHtml), attachments: msg.attachments.map((a) => a.name) });
    if (saveToSent)
      mock.state.addMessage({ user: u.key, folderId: mock.state.folder(u.key, 'sentitems')!.id, subject: msg.subject, bodyHtml: msg.bodyHtml, from: meAddress(u), to: msg.to, cc: msg.cc, receivedDateTime: iso(now), sentDateTime: iso(now), isRead: true, isDraft: false, importance: 'normal', conversationId: msg.conversationId, categories: [], label: null, attachments: msg.attachments }, now);
    for (const a of [...msg.to, ...msg.cc]) {
      const rcpt = mock.state.user(a.address);
      if (!rcpt) continue;
      mock.state.addMessage({ user: rcpt.key, folderId: mock.state.folder(rcpt.key, 'inbox')!.id, subject: msg.subject, bodyHtml: msg.bodyHtml, from: meAddress(u), to: msg.to, cc: msg.cc, receivedDateTime: iso(now), sentDateTime: iso(now), isRead: false, isDraft: false, importance: 'normal', conversationId: msg.conversationId, categories: [], label: null, attachments: msg.attachments }, now);
    }
  }
  r.post('/me/sendMail', (req: Req, res) => {
    const b = (req.body ?? {}) as { message?: Record<string, unknown>; saveToSentItems?: boolean };
    const msg = b.message;
    if (!msg) return graphError(res, 400, 'ErrorInvalidRequest', 'The message is missing.');
    const to = toAddresses(msg.toRecipients), cc = toAddresses(msg.ccRecipients);
    if (to.length + cc.length === 0) return graphError(res, 400, 'ErrorInvalidRecipients', 'At least one recipient is not valid.');
    const attachments = Array.isArray(msg.attachments)
      ? msg.attachments.map((a: Record<string, unknown>) => ({ id: mock.state.nextId('attachment'), name: String(a.name ?? ''), contentType: String(a.contentType ?? 'application/octet-stream'), bytes: Buffer.from(String(a.contentBytes ?? ''), 'base64'), lastModifiedDateTime: iso(mock.now()) }))
      : [];
    deliver(req.user!, { subject: String(msg.subject ?? ''), bodyHtml: bodyOf(msg.body), to, cc, attachments, conversationId: mock.state.nextId('conversation') }, b.saveToSentItems !== false);
    res.status(202).end();
  });
  r.post('/me/messages/:id/send', (req: Req, res) => {
    const m = findMessage(req, res);
    if (!m) return;
    if (!m.isDraft) return graphError(res, 400, 'ErrorInvalidOperation', 'Only drafts can be sent.');
    if (m.to.length + m.cc.length === 0) return graphError(res, 400, 'ErrorInvalidRecipients', 'At least one recipient is not valid.');
    mock.state.removeMessage(m);
    deliver(req.user!, m, true);
    res.status(202).end();
  });

  r.post('/me/messages/:id/move', (req: Req, res) => {
    const m = findMessage(req, res);
    if (!m) return;
    const dest = mock.state.folder(req.user!.key, String((req.body as { destinationId?: string })?.destinationId ?? ''));
    if (!dest) return graphError(res, 400, 'ErrorInvalidIdMalformed', 'The destination folder is not valid.');
    const moved = mock.state.moveMessage(m, dest.id, mock.now());
    res.status(201).json(messageJson(mock, moved, viewOf(req)));
  });
  r.delete('/me/messages/:id', (req: Req, res) => {
    const m = findMessage(req, res);
    if (!m) return;
    mock.state.removeMessage(m);
    res.status(204).end();
  });

  // ---- calendar ----
  r.get('/me/calendars', (req: Req, res) => {
    const u = req.user!;
    res.json({ '@odata.context': `${base(req)}/$metadata#users('${u.id}')/calendars`, value: [{ id: `${u.id}-calendar`, name: 'Calendar', color: 'auto', isDefaultCalendar: true, canEdit: true, owner: { name: u.displayName, address: u.upn } }] });
  });
  const windowOf = (req: Request, res: Response): [number, number] | undefined => {
    const s = Date.parse(String(req.query.startDateTime ?? '')), e = Date.parse(String(req.query.endDateTime ?? ''));
    if (Number.isNaN(s) || Number.isNaN(e)) {
      graphError(res, 400, 'ErrorInvalidParameter', 'This request requires a time window specified by the query string parameters StartDateTime and EndDateTime.');
      return undefined;
    }
    return [s, e];
  };
  function expanded(user: string, from: number, to: number): { e: CalEvent; occ?: { id: string; start: string; end: string } }[] {
    const out: { e: CalEvent; occ?: { id: string; start: string; end: string } }[] = [];
    for (const e of mock.state.events.filter((x) => x.user === user))
      for (const i of instances(mock, e, from, to)) out.push(i.occurrence ? { e, occ: i } : { e });
    return out.sort((a, b) => (a.occ?.start ?? a.e.start).localeCompare(b.occ?.start ?? b.e.start));
  }
  r.get('/me/calendarView', odata((req: Req, res) => {
    const w = windowOf(req, res);
    if (!w) return;
    let items = expanded(req.user!.key, w[0], w[1]).map((x) => eventJson(mock, x.e, viewOf(req), x.occ));
    if (req.query.$filter) { const n = parseFilter(String(req.query.$filter)); items = items.filter((i) => evaluate(n, i)); }
    if (req.query.$orderby) items = applyOrderBy(items, String(req.query.$orderby));
    page(req, res, items, 'users/calendarView', 10);
  }));
  r.get('/me/calendarView/delta', (req: Req, res) => {
    const user = req.user!.key;
    const tokenId = (req.query.$deltatoken ?? req.query.$skiptoken) as string | undefined;
    type Cont = { user: string; from: number; to: number; since: number; upto: number; offset: number; generation: number; select?: string; label: boolean };
    let c: Cont;
    if (tokenId) {
      const found = mock.continuations.get(tokenId) as Cont | undefined;
      if (!found || found.user !== user || found.generation !== mock.state.generation || mock.takeFault(req, ['delta_expired'])) {
        res.locals.fault = 'delta_expired';
        return graphError(res, 410, 'syncStateNotFound', 'The sync state generation is not found. Discard the delta token and start a full sync.');
      }
      c = req.query.$deltatoken ? { ...found, since: found.upto, upto: mock.state.seq, offset: 0 } : found;
    } else {
      const w = windowOf(req, res);
      if (!w) return;
      c = { user, from: w[0], to: w[1], since: 0, upto: mock.state.seq, offset: 0, generation: mock.state.generation, select: req.query.$select as string | undefined, label: viewOf(req).label };
    }
    const q: View = { ...viewOf(req), select: c.select, label: c.label };
    const changed = mock.state.events.filter((e) => e.user === user && e.seq > c.since && e.seq <= c.upto);
    const all: Record<string, unknown>[] = [];
    for (const e of changed) for (const i of instances(mock, e, c.from, c.to)) all.push({ '@odata.type': '#microsoft.graph.event', ...eventJson(mock, e, q, i.occurrence ? i : undefined) });
    if (c.since > 0)
      for (const t of mock.state.tombstones.filter((x) => x.kind === 'event' && x.user === user && x.seq > c.since && x.seq <= c.upto)) {
        const gone = mock.state.removedEvents.get(t.id);
        const ids = gone ? instances(mock, gone, c.from, c.to).map((i) => i.id) : [t.id];
        for (const id of ids) all.push({ '@odata.type': '#microsoft.graph.event', id, '@removed': { reason: 'deleted' } });
      }
    const size = maxPageSize(req, 10);
    const slice = all.slice(c.offset, c.offset + size);
    const body: Record<string, unknown> = { '@odata.context': `${base(req)}/$metadata#Collection(event)`, value: slice };
    const token = mock.nextToken('delta');
    if (c.offset + size < all.length) {
      mock.continuations.set(token, { ...c, offset: c.offset + size });
      body['@odata.nextLink'] = `${base(req)}/me/calendarView/delta?$skiptoken=${token}`;
    } else {
      mock.continuations.set(token, { ...c, since: c.upto, offset: 0 });
      body['@odata.deltaLink'] = `${base(req)}/me/calendarView/delta?$deltatoken=${token}`;
    }
    res.json(body);
  });

  const findEvent = (req: Req, res: Response): { e: CalEvent; occ?: { id: string; start: string; end: string } } | undefined => {
    const id = String(req.params.id);
    const user = req.user!.key;
    const e = mock.state.events.find((x) => x.user === user && x.id === id);
    if (e) return { e };
    const occ = /^(.*)_occ_(\d{8})$/.exec(id);
    const master = occ ? mock.state.events.find((x) => x.user === user && x.id === occ[1]) : undefined;
    if (master && occ) {
      const day = Date.parse(`${occ[2]!.slice(0, 4)}-${occ[2]!.slice(4, 6)}-${occ[2]!.slice(6)}T00:00:00Z`);
      const i = instances(mock, master, day, day + 86_400_000).find((x) => x.id === id);
      if (i) return { e: master, occ: i };
    }
    graphError(res, 404, 'ErrorItemNotFound', 'The specified object was not found in the store.');
    return undefined;
  };
  r.get('/me/events/:id', (req: Req, res) => {
    const f = findEvent(req, res);
    if (f) res.json({ '@odata.context': `${base(req)}/$metadata#users('${req.user!.id}')/events/$entity`, ...eventJson(mock, f.e, viewOf(req), f.occ) });
  });

  const timeOf = (v: unknown): string | null => {
    const o = (v ?? {}) as { dateTime?: string; timeZone?: string };
    if (!o.dateTime) return null;
    const zone = (o.timeZone ?? 'UTC').toUpperCase();
    const s = /(Z|[+-]\d{2}:\d{2})$/.test(o.dateTime) || zone !== 'UTC' ? o.dateTime : o.dateTime + 'Z';
    const t = Date.parse(s);
    return Number.isNaN(t) ? null : iso(new Date(t));
  };
  const attendeesOf = (v: unknown): Attendee[] =>
    Array.isArray(v)
      ? v.map((x: { emailAddress?: { name?: string; address?: string }; type?: string }) => ({ name: x.emailAddress?.name ?? '', address: x.emailAddress?.address ?? '', type: x.type === 'optional' ? ('optional' as const) : ('required' as const), response: 'none' })).filter((a) => a.address)
      : [];
  /** Invitations for the attendees: captured, and a copy in the calendar of each internal attendee. */
  function invite(organizer: User, e: CalEvent, kind: 'invitation' | 'cancellation'): void {
    const now = mock.now();
    if (e.attendees.length === 0) return;
    mock.state.captures.push({ kind, at: iso(now), from: organizer.upn, to: e.attendees.map((a) => a.address), cc: [], subject: (kind === 'cancellation' ? 'Canceled: ' : '') + e.subject, body: htmlToText(e.bodyHtml), attachments: [], event_id: e.id });
    for (const a of e.attendees) {
      const u = mock.state.user(a.address);
      if (!u || u.key === organizer.key) continue;
      const copy = mock.state.events.find((x) => x.user === u.key && x.iCalUId === e.iCalUId);
      if (kind === 'cancellation') {
        if (copy) { copy.isCancelled = true; copy.subject = 'Canceled: ' + copy.subject.replace(/^Canceled: /, ''); mock.state.touchEvent(copy, now); }
        continue;
      }
      if (copy) {
        Object.assign(copy, { subject: e.subject, bodyHtml: e.bodyHtml, start: e.start, end: e.end, location: e.location, attendees: e.attendees.map((x) => ({ ...x })) });
        mock.state.touchEvent(copy, now);
      } else {
        mock.state.events.push({ ...e, id: mock.state.nextId('event'), user: u.key, isOrganizer: false, response: 'notResponded', attendees: e.attendees.map((x) => ({ ...x })), seq: mock.state.bump(), createdDateTime: iso(now), lastModifiedDateTime: iso(now) });
      }
    }
  }
  const transactions = new Map<string, string>();
  r.post('/me/events', (req: Req, res) => {
    const u = req.user!, b = (req.body ?? {}) as Record<string, unknown>, now = mock.now();
    if (typeof b.transactionId === 'string' && transactions.has(`${u.key}|${b.transactionId}`)) {
      const existing = mock.state.events.find((x) => x.id === transactions.get(`${u.key}|${b.transactionId}`));
      if (existing) return res.status(201).json(eventJson(mock, existing, viewOf(req)));
    }
    const start = timeOf(b.start), end = timeOf(b.end);
    if (!start || !end || end <= start) return graphError(res, 400, 'ErrorInvalidRequest', 'The start and end of the event are not valid.');
    const e: CalEvent = {
      id: mock.state.nextId('event'), user: u.key, subject: String(b.subject ?? ''), bodyHtml: bodyOf(b.body), start, end, isAllDay: !!b.isAllDay,
      location: String((b.location as { displayName?: string } | undefined)?.displayName ?? ''), organizer: meAddress(u), attendees: attendeesOf(b.attendees),
      isOrganizer: true, response: 'organizer', sensitivity: String(b.sensitivity ?? 'normal'), showAs: String(b.showAs ?? 'busy'), categories: [], label: null,
      recurrence: null, isCancelled: false, createdDateTime: iso(now), lastModifiedDateTime: iso(now), seq: mock.state.bump(), iCalUId: mock.state.nextId('ical'),
    };
    if (b.isOnlineMeeting === true) e.onlineMeetingUrl = teamsJoinUrl(e.iCalUId);
    mock.state.events.push(e);
    if (typeof b.transactionId === 'string') transactions.set(`${u.key}|${b.transactionId}`, e.id);
    invite(u, e, 'invitation');
    res.status(201).json(eventJson(mock, e, viewOf(req)));
  });
  r.patch('/me/events/:id', (req: Req, res) => {
    const f = findEvent(req, res);
    if (!f) return;
    const e = f.e, b = (req.body ?? {}) as Record<string, unknown>;
    if (!e.isOrganizer && ['subject', 'start', 'end', 'attendees', 'location', 'body'].some((k) => k in b)) return graphError(res, 403, 'ErrorAccessDenied', 'Only the organizer can change the meeting.');
    if ('subject' in b) e.subject = String(b.subject ?? '');
    if ('body' in b) e.bodyHtml = bodyOf(b.body);
    if ('start' in b) e.start = timeOf(b.start) ?? e.start;
    if ('end' in b) e.end = timeOf(b.end) ?? e.end;
    if ('location' in b) e.location = String((b.location as { displayName?: string } | undefined)?.displayName ?? '');
    if ('attendees' in b) e.attendees = attendeesOf(b.attendees);
    if ('showAs' in b) e.showAs = String(b.showAs);
    if ('categories' in b && Array.isArray(b.categories)) e.categories = b.categories.map(String);
    mock.state.touchEvent(e, mock.now());
    if (e.isOrganizer && ['subject', 'start', 'end', 'attendees', 'location', 'body'].some((k) => k in b)) invite(req.user!, e, 'invitation');
    res.json(eventJson(mock, e, viewOf(req)));
  });
  r.delete('/me/events/:id', (req: Req, res) => {
    const f = findEvent(req, res);
    if (!f) return;
    if (f.e.isOrganizer) invite(req.user!, f.e, 'cancellation');
    mock.state.removeEvent(f.e);
    res.status(204).end();
  });
  for (const [path, response] of [['accept', 'accepted'], ['tentativelyAccept', 'tentativelyAccepted'], ['decline', 'declined']] as const) {
    r.post(`/me/events/:id/${path}`, (req: Req, res) => {
      const f = findEvent(req, res);
      if (!f) return;
      const e = f.e, u = req.user!, b = (req.body ?? {}) as { comment?: string; sendResponse?: boolean };
      if (e.isOrganizer) return graphError(res, 400, 'ErrorInvalidRequest', 'Your request can\'t be completed. You can\'t respond to a meeting you organized.');
      const now = mock.now();
      e.response = response;
      if (b.sendResponse !== false) {
        mock.state.captures.push({ kind: 'response', at: iso(now), from: u.upn, to: [e.organizer.address], cc: [], subject: `${response === 'accepted' ? 'Accepted' : response === 'declined' ? 'Declined' : 'Tentative'}: ${e.subject}`, body: b.comment ?? '', attachments: [], event_id: e.id, response, comment: b.comment ?? '' });
        const org = mock.state.user(e.organizer.address);
        const orgCopy = org && mock.state.events.find((x) => x.user === org.key && x.iCalUId === e.iCalUId);
        if (orgCopy) {
          const a = orgCopy.attendees.find((x) => x.address.toLowerCase() === u.upn.toLowerCase());
          if (a) { a.response = response; mock.state.touchEvent(orgCopy, now); }
        }
      }
      if (response === 'declined') mock.state.removeEvent(e);
      else mock.state.touchEvent(e, now);
      res.status(202).end();
    });
  }

  // Free/busy (getSchedule): fixture users' calendars; anyone else is unknown.
  r.post('/me/calendar/getSchedule', (req: Req, res) => {
    const b = (req.body ?? {}) as { schedules?: string[]; startTime?: unknown; endTime?: unknown; availabilityViewInterval?: number };
    const from = timeOf(b.startTime), to = timeOf(b.endTime);
    if (!Array.isArray(b.schedules) || !from || !to) return graphError(res, 400, 'ErrorInvalidParameter', 'schedules, startTime and endTime are required.');
    const interval = Math.max(5, Math.min(1440, b.availabilityViewInterval ?? 30));
    const f = Date.parse(from), t = Date.parse(to);
    const value = b.schedules.map((addr) => {
      const u = mock.state.user(addr);
      if (!u) return { scheduleId: addr, availabilityView: '', scheduleItems: [], error: { message: `The mailbox ${addr} was not found.`, responseCode: 'ErrorMailRecipientNotFound' } };
      const items = expanded(u.key, f, t).filter((x) => !x.e.isCancelled && x.e.showAs !== 'free');
      const view: string[] = [];
      const code: Record<string, string> = { tentative: '1', busy: '2', oof: '3', workingElsewhere: '4' };
      for (let s = f; s < t; s += interval * 60_000) {
        let v = '0';
        for (const x of items) {
          const xs = Date.parse(x.occ?.start ?? x.e.start), xe = Date.parse(x.occ?.end ?? x.e.end);
          if (xs < s + interval * 60_000 && xe > s && (code[x.e.showAs] ?? '2') > v) v = code[x.e.showAs] ?? '2';
        }
        view.push(v);
      }
      return {
        scheduleId: addr,
        availabilityView: view.join(''),
        scheduleItems: items.map((x) => ({ isPrivate: x.e.sensitivity === 'private', status: x.e.showAs, subject: x.e.sensitivity === 'private' ? undefined : x.e.subject, location: x.e.sensitivity === 'private' ? undefined : x.e.location, start: dt(x.occ?.start ?? x.e.start), end: dt(x.occ?.end ?? x.e.end) })),
        workingHours: { daysOfWeek: ['monday', 'tuesday', 'wednesday', 'thursday', 'friday'], startTime: '08:00:00.0000000', endTime: '17:00:00.0000000', timeZone: { name: u.timeZone } },
      };
    });
    res.json({ '@odata.context': `${base(req)}/$metadata#Collection(microsoft.graph.scheduleInformation)`, value });
  });

  // Anything else is not mocked (Unsupported Endpoint).
  r.use((req: Req, res: Response) => graphError(res, 501, 'NotMocked', `dev/graph-mock does not implement ${req.method} /v1.0${req.path}.`));
  return r;
}
