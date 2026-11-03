// The test control API under /_mock on its own port (dev/graph-mock Test Inspection And Control API, Fault Injection,
// Mock OAuth Fault Injection): reset and fixture sets, delivering mail and changing events to drive delta sync,
// captured sends, the request log, Graph and OAuth faults, token lifetimes.
import { Router, type Request, type Response } from 'express';
import type { GraphMock, Fault, OAuthFault } from './mock.ts';
import { iso, resolveTime, makePdf, makeDocx, teamsJoinUrl, type Attachment, type CalEvent } from './state.ts';

const FAULT_KINDS = ['throttle', 'server_error', 'delta_expired', 'slow', 'malformed'];
const OAUTH_FAULT_KINDS = ['interaction_required', 'consent_required', 'invalid_grant', 'expired_token'];

function bad(res: Response, message: string): void {
  res.status(400).json({ error: message });
}

export function controlRouter(mock: GraphMock): Router {
  const r = Router();
  const when = (spec: unknown, fallback: Date): Date => (typeof spec === 'string' && spec ? resolveTime(mock.state.anchor, spec) : fallback);

  r.get('/health', (_req, res) => {
    res.json({ ok: true, graph: mock.graphBase, control: mock.controlBase, fixtures: mock.fixturesName, seed: mock.state.seed, anchor: mock.state.anchor.toISOString() });
  });
  r.get('/users', (_req, res) => {
    res.json(mock.state.users.map((u) => ({ key: u.key, upn: u.upn, id: u.id, displayName: u.displayName, default: u.isDefault })));
  });
  r.post('/reset', (req: Request, res: Response) => {
    const b = (req.body ?? {}) as { seed?: string; fixtures?: string; anchor?: string };
    try {
      mock.reset(b);
    } catch (e) {
      return bad(res, (e as Error).message);
    }
    res.json({ ok: true, fixtures: mock.fixturesName, seed: mock.state.seed, anchor: mock.state.anchor.toISOString() });
  });

  // ---- driving delta sync ----
  r.post('/users/:user/messages', (req: Request, res: Response) => {
    const u = mock.state.user(String(req.params.user));
    if (!u) return bad(res, 'no such user');
    const b = (req.body ?? {}) as { from?: string; to?: string[]; cc?: string[]; subject?: string; body?: string; folder?: string; label?: string; categories?: string[]; received?: string; attachments?: { name: string; text: string }[] };
    const folder = mock.state.folder(u.key, b.folder ?? 'inbox');
    if (!folder) return bad(res, 'no such folder');
    const now = mock.now();
    const received = when(b.received, now);
    const attachments: Attachment[] = (b.attachments ?? []).map((a) => {
      const bytes = a.name.endsWith('.pdf') ? makePdf(a.text) : a.name.endsWith('.docx') ? makeDocx(a.text) : Buffer.from(a.text, 'utf8');
      const contentType = a.name.endsWith('.pdf') ? 'application/pdf' : a.name.endsWith('.docx') ? 'application/vnd.openxmlformats-officedocument.wordprocessingml.document' : 'text/plain';
      return { id: mock.state.nextId('attachment'), name: a.name, contentType, bytes, lastModifiedDateTime: iso(received) };
    });
    const m = mock.state.addMessage({
      user: u.key, folderId: folder.id, subject: b.subject ?? '', bodyHtml: b.body ?? '', from: mock.state.address(b.from ?? 'Sender <sender@example.com>'),
      to: (b.to ?? [u.key]).map((t) => mock.state.address(t)), cc: (b.cc ?? []).map((t) => mock.state.address(t)), receivedDateTime: iso(received),
      sentDateTime: iso(received), isRead: false, isDraft: false, importance: 'normal', conversationId: mock.state.nextId('conversation'),
      categories: b.categories ?? [], label: mock.state.labelId(b.label), attachments,
    }, now);
    res.status(201).json({ id: m.id });
  });
  r.delete('/users/:user/messages/:id', (req, res) => {
    const m = mock.state.messages.find((x) => x.id === String(req.params.id) && x.user === mock.state.user(String(req.params.user))?.key);
    if (!m) return res.status(404).json({ error: 'no such message' });
    mock.state.removeMessage(m);
    res.status(204).end();
  });
  r.post('/users/:user/events', (req: Request, res: Response) => {
    const u = mock.state.user(String(req.params.user));
    if (!u) return bad(res, 'no such user');
    const b = (req.body ?? {}) as { subject?: string; body?: string; start?: string; minutes?: number; organizer?: string; attendees?: string[]; private?: boolean; label?: string; location?: string; showAs?: string; online?: boolean };
    const now = mock.now();
    const start = when(b.start, new Date(now.getTime() + 86_400_000));
    const organizer = mock.state.address(b.organizer ?? u.key);
    const isOrganizer = mock.state.user(organizer.address)?.key === u.key;
    const e: CalEvent = {
      id: mock.state.nextId('event'), user: u.key, subject: b.subject ?? '', bodyHtml: b.body ?? '', start: iso(start), end: iso(new Date(start.getTime() + (b.minutes ?? 30) * 60_000)),
      isAllDay: false, location: b.location ?? '', organizer, attendees: (b.attendees ?? []).map((a) => ({ ...mock.state.address(a), type: 'required' as const, response: 'none' })),
      isOrganizer, response: isOrganizer ? 'organizer' : 'notResponded', sensitivity: b.private ? 'private' : 'normal', showAs: b.showAs ?? 'busy', categories: [],
      label: mock.state.labelId(b.label), recurrence: null, isCancelled: false, createdDateTime: iso(now), lastModifiedDateTime: iso(now), seq: mock.state.bump(), iCalUId: mock.state.nextId('ical'),
    };
    if (b.online) e.onlineMeetingUrl = teamsJoinUrl(e.iCalUId);
    mock.state.events.push(e);
    res.status(201).json({ id: e.id });
  });
  /** The organiser changes an event (reschedule, rename, label): every copy with the same iCalUId changes. */
  r.patch('/users/:user/events/:id', (req: Request, res: Response) => {
    const u = mock.state.user(String(req.params.user));
    const e = u && mock.state.events.find((x) => x.user === u.key && x.id === String(req.params.id));
    if (!e) return res.status(404).json({ error: 'no such event' });
    const b = (req.body ?? {}) as { subject?: string; start?: string; minutes?: number; label?: string | null; private?: boolean; location?: string };
    const now = mock.now();
    for (const copy of mock.state.events.filter((x) => x.iCalUId === e.iCalUId)) {
      if (b.subject !== undefined) copy.subject = b.subject;
      if (b.location !== undefined) copy.location = b.location;
      if (b.start !== undefined || b.minutes !== undefined) {
        const s = b.start !== undefined ? when(b.start, new Date(copy.start)) : new Date(copy.start);
        const len = b.minutes !== undefined ? b.minutes * 60_000 : Date.parse(copy.end) - Date.parse(copy.start);
        copy.start = iso(s);
        copy.end = iso(new Date(s.getTime() + len));
      }
      if (b.label !== undefined) copy.label = mock.state.labelId(b.label);
      if (b.private !== undefined) copy.sensitivity = b.private ? 'private' : 'normal';
      mock.state.touchEvent(copy, now);
    }
    res.json({ ok: true });
  });
  /** The organiser cancels: attendee copies become cancelled, the organiser's copy is removed. */
  r.post('/users/:user/events/:id/cancel', (req: Request, res: Response) => {
    const u = mock.state.user(String(req.params.user));
    const e = u && mock.state.events.find((x) => x.user === u.key && x.id === String(req.params.id));
    if (!e) return res.status(404).json({ error: 'no such event' });
    const now = mock.now();
    for (const copy of mock.state.events.filter((x) => x.iCalUId === e.iCalUId)) {
      if (copy.isOrganizer) mock.state.removeEvent(copy);
      else {
        copy.isCancelled = true;
        copy.subject = 'Canceled: ' + copy.subject.replace(/^Canceled: /, '');
        mock.state.touchEvent(copy, now);
      }
    }
    res.json({ ok: true });
  });

  // ---- inspection ----
  r.get('/sends', (req, res) => {
    const kind = req.query.kind as string | undefined;
    res.json(mock.state.captures.filter((c) => !kind || c.kind === kind));
  });
  r.delete('/sends', (_req, res) => {
    mock.state.captures = [];
    res.status(204).end();
  });
  r.get('/requests', (req, res) => {
    const surface = req.query.surface as string | undefined;
    res.json(mock.requests.filter((e) => !surface || e.surface === surface));
  });
  r.delete('/requests', (_req, res) => {
    mock.requests = [];
    res.status(204).end();
  });

  // ---- faults ----
  r.post('/faults', (req: Request, res: Response) => {
    const b = (req.body ?? {}) as Partial<Fault> & { count?: number };
    if (!b.kind || !FAULT_KINDS.includes(b.kind)) return bad(res, `kind must be one of ${FAULT_KINDS.join(', ')}`);
    const f = mock.addFault({ kind: b.kind, method: b.method, path: b.path, remaining: b.count ?? 1, retry_after: b.retry_after, delay_ms: b.delay_ms, status: b.status });
    res.status(201).json(f);
  });
  r.get('/faults', (_req, res) => {
    res.json({ graph: mock.faults, oauth: mock.oauthFaults });
  });
  r.delete('/faults', (_req, res) => {
    mock.faults = [];
    mock.oauthFaults = [];
    res.status(204).end();
  });
  /** Every delta token issued so far is expired. */
  r.post('/delta/expire', (_req, res) => {
    mock.state.generation++;
    res.json({ generation: mock.state.generation });
  });
  r.post('/oauth/faults', (req: Request, res: Response) => {
    const b = (req.body ?? {}) as Partial<OAuthFault> & { count?: number };
    if (!b.kind || !OAUTH_FAULT_KINDS.includes(b.kind)) return bad(res, `kind must be one of ${OAUTH_FAULT_KINDS.join(', ')}`);
    const f = mock.addOAuthFault({ kind: b.kind, user: b.user, step: b.step, remaining: b.count ?? 1 });
    res.status(201).json(f);
  });
  r.post('/oauth/config', (req: Request, res: Response) => {
    const b = (req.body ?? {}) as Partial<GraphMock['oauthConfig']>;
    if (typeof b.access_token_lifetime_s === 'number') mock.oauthConfig.access_token_lifetime_s = b.access_token_lifetime_s;
    if (typeof b.refresh_token_lifetime_s === 'number') mock.oauthConfig.refresh_token_lifetime_s = b.refresh_token_lifetime_s;
    res.json(mock.oauthConfig);
  });
  /** Revokes the refresh tokens of one user, or of everyone. */
  r.post('/oauth/revoke', (req: Request, res: Response) => {
    const user = (req.body as { user?: string } | undefined)?.user;
    const key = user ? mock.state.user(user)?.key : undefined;
    for (const [t, v] of mock.refreshTokens) if (!user || v.user === key) mock.refreshTokens.delete(t);
    res.json({ ok: true });
  });

  return r;
}
