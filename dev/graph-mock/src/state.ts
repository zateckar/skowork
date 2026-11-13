// The mock's mailbox and calendar state (dev/graph-mock Deterministic Seeded Fixtures): users, folders, messages,
// attachments and events, built from a fixture file and a seed. Identifiers and dates depend only on the fixture set,
// the seed and the anchor, so two mocks started alike return identical data.
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { zipSync } from './zip.ts';

export const LABEL_PROPERTY = 'String {00020386-0000-0000-C000-000000000046} Name msip_labels';

export interface Address { name: string; address: string }
export interface Attachment { id: string; name: string; contentType: string; bytes: Buffer; lastModifiedDateTime: string }
export interface Message {
  id: string; user: string; folderId: string; subject: string; bodyHtml: string; from: Address; to: Address[]; cc: Address[];
  receivedDateTime: string; sentDateTime: string; createdDateTime: string; lastModifiedDateTime: string; isRead: boolean; isDraft: boolean;
  importance: string; conversationId: string; categories: string[]; label: string | null; attachments: Attachment[];
  internetMessageId: string; seq: number;
}
export interface Attendee { name: string; address: string; type: 'required' | 'optional'; response: string }
export interface Recurrence { type: 'daily' | 'weekly'; interval: number; daysOfWeek: string[]; startDate: string; endDate: string | null }
export interface CalEvent {
  id: string; user: string; subject: string; bodyHtml: string; start: string; end: string; isAllDay: boolean; location: string;
  organizer: Address; attendees: Attendee[]; isOrganizer: boolean; response: string; sensitivity: string; showAs: string;
  categories: string[]; label: string | null; recurrence: Recurrence | null; isCancelled: boolean; createdDateTime: string;
  lastModifiedDateTime: string; seq: number; iCalUId: string;
  /** The join link of an online meeting; absent for a meeting in a room. */
  onlineMeetingUrl?: string | null;
}
export interface Folder { id: string; user: string; displayName: string; wellKnownName: string | null; parentFolderId: string }
export interface User { key: string; id: string; displayName: string; upn: string; timeZone: string; language: string; isDefault: boolean; department: string | null; jobTitle: string | null; manager: string | null }
export interface Tombstone { id: string; user: string; kind: 'message' | 'event'; folderId: string; seq: number }
export interface Capture { kind: 'mail' | 'invitation' | 'response' | 'cancellation'; at: string; from: string; to: string[]; cc: string[]; subject: string; body: string; attachments: string[]; event_id?: string; response?: string; comment?: string }

export const WELL_KNOWN = ['inbox', 'drafts', 'sentitems', 'deleteditems', 'archive', 'junkemail'] as const;
const FOLDER_NAMES: Record<string, string> = { inbox: 'Inbox', drafts: 'Drafts', sentitems: 'Sent Items', deleteditems: 'Deleted Items', archive: 'Archive', junkemail: 'Junk Email' };

/** A Teams-shaped join link for an online meeting of the mock. */
export function teamsJoinUrl(key: string): string {
  return `https://teams.microsoft.com/l/meetup-join/19%3ameeting_${encodeURIComponent(key.slice(-16))}%40thread.v2/0`;
}

export function graphId(seed: string, ...parts: string[]): string {
  return 'AAMkAG' + createHash('sha256').update([seed, ...parts].join('|')).digest('base64url').slice(0, 42);
}

/** "+2d 10:00" (the anchor's day plus 2 days at 10:00 UTC), "-26h", "+0d", or an ISO date-time. */
export function resolveTime(anchor: Date, spec: string): Date {
  const d = /^([+-]\d+)d(?:\s+(\d{1,2}):(\d{2}))?$/.exec(spec);
  if (d) {
    const t = new Date(Date.UTC(anchor.getUTCFullYear(), anchor.getUTCMonth(), anchor.getUTCDate() + Number(d[1])));
    if (d[2] !== undefined) t.setUTCHours(Number(d[2]), Number(d[3]));
    else t.setUTCHours(anchor.getUTCHours(), anchor.getUTCMinutes());
    return t;
  }
  const h = /^([+-]\d+)h$/.exec(spec);
  if (h) return new Date(anchor.getTime() + Number(h[1]) * 3_600_000);
  const iso = new Date(spec);
  if (Number.isNaN(iso.getTime())) throw new Error(`bad time '${spec}'`);
  return iso;
}

export function iso(d: Date): string {
  return d.toISOString().replace(/\.\d{3}Z$/, 'Z');
}

// ---- generated attachment documents --------------------------------------------------------------------------------

/** A minimal one-page PDF whose text is in an uncompressed content stream. */
export function makePdf(text: string): Buffer {
  const esc = (s: string) => s.replace(/[\\()]/g, (c) => '\\' + c);
  const lines = text.split('\n');
  const content = 'BT /F1 12 Tf 72 720 Td 14 TL\n' + lines.map((l) => `(${esc(l)}) '`).join('\n') + '\nET\n';
  const objs = [
    '<< /Type /Catalog /Pages 2 0 R >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>',
    `<< /Length ${Buffer.byteLength(content, 'latin1')} >>\nstream\n${content}endstream`,
    '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>',
  ];
  let out = '%PDF-1.4\n';
  const offsets: number[] = [];
  objs.forEach((o, i) => {
    offsets.push(Buffer.byteLength(out, 'latin1'));
    out += `${i + 1} 0 obj\n${o}\nendobj\n`;
  });
  const xref = Buffer.byteLength(out, 'latin1');
  out += `xref\n0 ${objs.length + 1}\n0000000000 65535 f \n` + offsets.map((o) => String(o).padStart(10, '0') + ' 00000 n \n').join('');
  out += `trailer\n<< /Size ${objs.length + 1} /Root 1 0 R >>\nstartxref\n${xref}\n%%EOF\n`;
  return Buffer.from(out, 'latin1');
}

/** A minimal .docx with one paragraph per line. */
export function makeDocx(text: string): Buffer {
  const xmlEsc = (s: string) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  const paras = text.split('\n').map((l) => `<w:p><w:r><w:t xml:space="preserve">${xmlEsc(l)}</w:t></w:r></w:p>`).join('');
  return zipSync([
    ['[Content_Types].xml', '<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>'],
    ['_rels/.rels', '<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>'],
    ['word/document.xml', `<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>${paras}</w:body></w:document>`],
  ]);
}

// ---- fixture files -------------------------------------------------------------------------------------------------

interface FixtureAttachment { name: string; contentType?: string; text: string; generate?: 'pdf' | 'docx' | 'text' }
interface FixtureMessage {
  user: string; key: string; folder?: string; from: string; to?: string[]; cc?: string[]; subject: string; body: string; received: string;
  label?: string | null; categories?: string[]; attachments?: FixtureAttachment[]; thread?: string; isRead?: boolean; importance?: string;
}
interface FixtureEvent {
  user: string; key: string; uid?: string; subject: string; body?: string; start: string; minutes: number; allDay?: boolean; location?: string;
  organizer?: string; attendees?: string[]; optional?: string[]; private?: boolean; label?: string | null; categories?: string[];
  recurrence?: { type: 'daily' | 'weekly'; interval?: number; daysOfWeek?: string[]; until?: string }; showAs?: string; response?: string;
  online?: boolean;
}
export interface FixtureSet {
  anchor: string;
  labels: Record<string, string>;
  users: { key: string; displayName: string; upn: string; timeZone: string; language: string; default?: boolean; department?: string; jobTitle?: string; manager?: string }[];
  folders?: { user: string; name: string }[];
  messages: FixtureMessage[];
  events: FixtureEvent[];
}

export function loadFixtureFile(path: string): FixtureSet {
  return JSON.parse(readFileSync(path, 'utf8')) as FixtureSet;
}

export class MockState {
  readonly seed: string;
  readonly anchor: Date;
  readonly labels: Record<string, string>;
  users: User[] = [];
  folders: Folder[] = [];
  messages: Message[] = [];
  events: CalEvent[] = [];
  tombstones: Tombstone[] = [];
  /** Removed events, so delta can report the removal of each occurrence of a series. */
  removedEvents = new Map<string, CalEvent>();
  captures: Capture[] = [];
  seq = 0;
  /** Delta tokens issued before this generation are expired (reset, or the delta-expiry fault). */
  generation = 1;
  #counter = 0;

  constructor(fixtures: FixtureSet, seed: string, anchor?: string) {
    this.seed = seed;
    this.anchor = new Date(anchor ?? fixtures.anchor);
    this.labels = fixtures.labels;
    for (const u of fixtures.users) {
      this.users.push({ key: u.key, id: graphId(seed, 'user', u.key), displayName: u.displayName, upn: u.upn, timeZone: u.timeZone, language: u.language, isDefault: !!u.default, department: u.department ?? null, jobTitle: u.jobTitle ?? null, manager: u.manager ?? null });
      const root = graphId(seed, 'folder', u.key, 'msgfolderroot');
      for (const w of WELL_KNOWN) this.folders.push({ id: graphId(seed, 'folder', u.key, w), user: u.key, displayName: FOLDER_NAMES[w]!, wellKnownName: w, parentFolderId: root });
    }
    for (const f of fixtures.folders ?? []) this.folders.push({ id: graphId(seed, 'folder', f.user, f.name), user: f.user, displayName: f.name, wellKnownName: null, parentFolderId: graphId(seed, 'folder', f.user, 'msgfolderroot') });
    for (const m of fixtures.messages) this.#addFixtureMessage(m);
    for (const e of fixtures.events) this.#addFixtureEvent(e);
  }

  nextId(kind: string): string {
    return graphId(this.seed, 'runtime', kind, String(++this.#counter));
  }
  bump(): number {
    return ++this.seq;
  }

  user(keyOrUpn: string): User | undefined {
    const k = keyOrUpn.toLowerCase();
    return this.users.find((u) => u.key === k || u.upn.toLowerCase() === k || u.id === keyOrUpn);
  }
  defaultUser(): User {
    return this.users.find((u) => u.isDefault) ?? this.users[0]!;
  }
  /** "eva" (a fixture user) or "Name <address>" (anyone else). */
  address(spec: string): Address {
    const u = this.user(spec);
    if (u) return { name: u.displayName, address: u.upn };
    const m = /^(.*)<([^>]+)>$/.exec(spec.trim());
    return m ? { name: m[1]!.trim(), address: m[2]!.trim() } : { name: spec, address: spec };
  }
  folder(user: string, idOrWellKnown: string): Folder | undefined {
    const k = idOrWellKnown.toLowerCase();
    return this.folders.find((f) => f.user === user && (f.id === idOrWellKnown || f.wellKnownName === k || f.displayName.toLowerCase() === k));
  }
  labelId(name: string | null | undefined): string | null {
    if (!name) return null;
    return this.labels[name] ?? name;
  }

  #addFixtureMessage(m: FixtureMessage): void {
    const received = resolveTime(this.anchor, m.received);
    const id = graphId(this.seed, 'message', m.user, m.key);
    const attachments = (m.attachments ?? []).map((a, i) => {
      const kind = a.generate ?? (a.name.endsWith('.pdf') ? 'pdf' : a.name.endsWith('.docx') ? 'docx' : 'text');
      const bytes = kind === 'pdf' ? makePdf(a.text) : kind === 'docx' ? makeDocx(a.text) : Buffer.from(a.text, 'utf8');
      const contentType = a.contentType ?? (kind === 'pdf' ? 'application/pdf' : kind === 'docx' ? 'application/vnd.openxmlformats-officedocument.wordprocessingml.document' : 'text/plain');
      return { id: graphId(this.seed, 'attachment', m.user, m.key, String(i)), name: a.name, contentType, bytes, lastModifiedDateTime: iso(received) };
    });
    this.messages.push({
      id, user: m.user, folderId: this.folder(m.user, m.folder ?? 'inbox')!.id, subject: m.subject, bodyHtml: m.body, from: this.address(m.from),
      to: (m.to ?? [m.user]).map((t) => this.address(t)), cc: (m.cc ?? []).map((t) => this.address(t)), receivedDateTime: iso(received),
      sentDateTime: iso(new Date(received.getTime() - 60_000)), createdDateTime: iso(received), lastModifiedDateTime: iso(received),
      isRead: m.isRead ?? false, isDraft: false, importance: m.importance ?? 'normal',
      conversationId: graphId(this.seed, 'conversation', m.user, m.thread ?? m.key), categories: m.categories ?? [], label: this.labelId(m.label),
      attachments, internetMessageId: `<${m.key}.${m.user}@mock.skowork>`, seq: this.bump(),
    });
  }

  #addFixtureEvent(e: FixtureEvent): void {
    const start = resolveTime(this.anchor, e.start);
    const end = new Date(start.getTime() + e.minutes * 60_000);
    const organizer = this.address(e.organizer ?? e.user);
    const attendees: Attendee[] = [
      ...(e.attendees ?? []).map((a) => ({ ...this.address(a), type: 'required' as const, response: 'none' })),
      ...(e.optional ?? []).map((a) => ({ ...this.address(a), type: 'optional' as const, response: 'none' })),
    ];
    const isOrganizer = !e.organizer || this.user(e.organizer)?.key === e.user;
    this.events.push({
      id: graphId(this.seed, 'event', e.user, e.key), user: e.user, subject: e.subject, bodyHtml: e.body ?? '', start: iso(start), end: iso(end),
      isAllDay: !!e.allDay, location: e.location ?? '', organizer, attendees, isOrganizer, response: isOrganizer ? 'organizer' : (e.response ?? 'notResponded'),
      sensitivity: e.private ? 'private' : 'normal', showAs: e.showAs ?? 'busy', categories: e.categories ?? [], label: this.labelId(e.label),
      recurrence: e.recurrence
        ? { type: e.recurrence.type, interval: e.recurrence.interval ?? 1, daysOfWeek: e.recurrence.daysOfWeek ?? [], startDate: iso(start).slice(0, 10), endDate: e.recurrence.until ? iso(resolveTime(this.anchor, e.recurrence.until)).slice(0, 10) : null }
        : null,
      isCancelled: false, createdDateTime: iso(new Date(start.getTime() - 7 * 86_400_000)), lastModifiedDateTime: iso(new Date(start.getTime() - 7 * 86_400_000)),
      seq: this.bump(), iCalUId: graphId(this.seed, 'ical', e.uid ?? e.key),
      onlineMeetingUrl: e.online ? teamsJoinUrl(graphId(this.seed, 'ical', e.uid ?? e.key)) : null,
    });
  }

  /** A new message in a user's folder (delivery, sent copies, drafts); returns it. */
  addMessage(m: Omit<Message, 'id' | 'seq' | 'internetMessageId' | 'createdDateTime' | 'lastModifiedDateTime'> & { id?: string }, now: Date): Message {
    const id = m.id ?? this.nextId('message');
    const msg: Message = { ...m, id, internetMessageId: `<${id.slice(-12)}@mock.skowork>`, createdDateTime: iso(now), lastModifiedDateTime: iso(now), seq: this.bump() };
    this.messages.push(msg);
    return msg;
  }
  removeMessage(m: Message): void {
    this.messages = this.messages.filter((x) => x !== m);
    this.tombstones.push({ id: m.id, user: m.user, kind: 'message', folderId: m.folderId, seq: this.bump() });
  }
  moveMessage(m: Message, folderId: string, now: Date): Message {
    // A move gives the message a new id in its new folder, as Exchange does; the old one is removed from the old folder.
    this.tombstones.push({ id: m.id, user: m.user, kind: 'message', folderId: m.folderId, seq: this.bump() });
    this.messages = this.messages.filter((x) => x !== m);
    const moved: Message = { ...m, id: this.nextId('message'), folderId, lastModifiedDateTime: iso(now), seq: this.bump() };
    this.messages.push(moved);
    return moved;
  }
  touchEvent(e: CalEvent, now: Date): void {
    e.seq = this.bump();
    e.lastModifiedDateTime = iso(now);
  }
  removeEvent(e: CalEvent): void {
    this.events = this.events.filter((x) => x !== e);
    this.removedEvents.set(e.id, e);
    this.tombstones.push({ id: e.id, user: e.user, kind: 'event', folderId: '', seq: this.bump() });
  }
}
