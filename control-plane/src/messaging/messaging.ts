// control-plane/user-messaging (add-user-messaging): the user directory, Presence, direct and team Chats, message
// relay through the push connection with catch-up by sequence number, read positions, retention and audit.
// Message content never reaches the audit log, telemetry or the admin console.
import { randomUUID } from 'node:crypto';
import { Router, type NextFunction, type Request, type Response } from 'express';
import type { Config } from '../config.ts';
import type { Db } from '../db.ts';
import { nowIso, tx } from '../db.ts';
import { ApiError, str } from '../http.ts';
import type { Audit } from '../identity/audit.ts';
import type { User } from '../identity/identity.ts';
import type { PushHub } from '../fleet/push.ts';

export type Presence = 'online' | 'away' | 'busy' | 'offline';
const MAX_TEAM_MEMBERS = 50;
const MESSAGE_ID = /^[A-Za-z0-9_-]{8,64}$/;

interface ChatRow {
  id: string;
  kind: 'direct' | 'team';
  name: string | null;
  pair_key: string | null;
  created_by: string;
  created_at: string;
  last_seq: number;
}

interface MessageRow {
  chat: string;
  seq: number;
  id: string;
  sender: string;
  text: string;
  at: string;
}

const lower = (s: string) => s.toLowerCase();

export class Messaging {
  readonly #cfg: Config;
  readonly #db: Db;
  readonly #audit: Audit;
  readonly #push: PushHub;
  readonly #mode: (u: User) => 'on' | 'off';
  readonly #known = new Map<string, Presence>();
  readonly #pending = new Map<string, NodeJS.Timeout>();
  readonly #timer: NodeJS.Timeout;

  constructor(cfg: Config, db: Db, audit: Audit, push: PushHub, mode: (u: User) => 'on' | 'off') {
    this.#cfg = cfg;
    this.#db = db;
    this.#audit = audit;
    this.#push = push;
    this.#mode = mode;
    push.onUserConnection = (u) => this.presenceMayHaveChanged(u);
    this.purge();
    this.#timer = setInterval(() => this.purge(), 3600_000);
    this.#timer.unref();
  }

  close() {
    clearInterval(this.#timer);
    for (const t of this.#pending.values()) clearTimeout(t);
  }

  // ---- users and Presence ---------------------------------------------------------------------------------------

  /** Every messaging request: refuses when policy.messaging.mode is off and keeps the user's profile current. */
  enter(user: User) {
    const on = this.#mode(user) === 'on';
    this.#db
      .prepare(
        `INSERT INTO user_profiles (user_name, display_name, messaging, last_seen) VALUES (?, ?, ?, ?)
         ON CONFLICT (user_name) DO UPDATE SET display_name = excluded.display_name, messaging = excluded.messaging, last_seen = excluded.last_seen`,
      )
      .run(user.userName, user.displayName || user.userName, on ? 1 : 0, nowIso());
    if (!on) throw new ApiError(403, 'MESSAGING_OFF', 'Messaging is switched off by policy.messaging.mode.');
  }

  presenceOf(userName: string): Presence {
    const installations = this.#push.connectedInstallations(userName);
    if (!installations.length) return 'offline';
    const p = this.#db.prepare('SELECT busy FROM user_profiles WHERE user_name = ?').get(userName) as { busy: number } | undefined;
    if (p?.busy) return 'busy';
    const idle = installations.every((id) => (this.#db.prepare('SELECT idle FROM presence_reports WHERE installation_id = ?').get(id) as { idle: number } | undefined)?.idle === 1);
    return idle ? 'away' : 'online';
  }

  /** Pushes presence-changed to the members of the user's Chats when the user's Presence changed (debounced). */
  presenceMayHaveChanged(userName: string) {
    const key = lower(userName);
    if (this.#pending.has(key)) return;
    const t = setTimeout(() => {
      this.#pending.delete(key);
      const now = this.presenceOf(userName);
      if (this.#known.get(key) === now) return;
      this.#known.set(key, now);
      for (const peer of this.#peers(userName)) this.#push.send('presence-changed', { user: peer }, { user_name: userName, presence: now }, 60);
    }, 200);
    t.unref();
    this.#pending.set(key, t);
  }

  #peers(userName: string): string[] {
    return (
      this.#db
        .prepare(
          `SELECT DISTINCT m2.user_name AS u FROM chat_members m1 JOIN chat_members m2 ON m2.chat = m1.chat
           WHERE m1.user_name = ? AND m2.user_name != m1.user_name`,
        )
        .all(userName) as { u: string }[]
    ).map((r) => r.u);
  }

  setPresence(user: User, installation: string | undefined, body: Record<string, unknown>) {
    this.enter(user);
    if (body.state !== undefined) {
      if (body.state !== 'active' && body.state !== 'idle') throw new ApiError(400, 'INVALID_REQUEST', 'state must be active or idle.');
      if (!installation) throw new ApiError(400, 'INVALID_REQUEST', 'The X-Skowork-Installation header is required with state.');
      this.#db
        .prepare(
          `INSERT INTO presence_reports (installation_id, user_name, idle, at) VALUES (?, ?, ?, ?)
           ON CONFLICT (installation_id) DO UPDATE SET user_name = excluded.user_name, idle = excluded.idle, at = excluded.at`,
        )
        .run(installation.toLowerCase(), user.userName, body.state === 'idle' ? 1 : 0, nowIso());
    }
    if (body.busy !== undefined) {
      if (typeof body.busy !== 'boolean') throw new ApiError(400, 'INVALID_REQUEST', 'busy must be true or false.');
      this.#db.prepare('UPDATE user_profiles SET busy = ? WHERE user_name = ?').run(body.busy ? 1 : 0, user.userName);
    }
    this.presenceMayHaveChanged(user.userName);
    return { presence: this.presenceOf(user.userName), busy: Boolean((this.#db.prepare('SELECT busy FROM user_profiles WHERE user_name = ?').get(user.userName) as { busy: number }).busy) };
  }

  #displayName(userName: string): string {
    return (this.#db.prepare('SELECT display_name FROM user_profiles WHERE user_name = ?').get(userName) as { display_name: string } | undefined)?.display_name ?? userName;
  }

  /** Signed-in users with messaging on and not denied, by prefix of user name or display name (User Directory). */
  directory(user: User, q: string | undefined) {
    this.enter(user);
    const like = `${(q ?? '').trim().toLowerCase().replace(/[%_\\]/g, (c) => '\\' + c)}%`;
    const rows = this.#db
      .prepare(
        `SELECT u.user_name, COALESCE(p.display_name, u.user_name) AS display_name FROM
           (SELECT user_name FROM installations WHERE revoked_at IS NULL UNION SELECT user_name FROM user_profiles) u
         LEFT JOIN user_profiles p ON p.user_name = u.user_name
         WHERE COALESCE(p.messaging, 1) = 1
           AND NOT EXISTS (SELECT 1 FROM deny_list d WHERE d.kind = 'user' AND lower(d.value) = lower(u.user_name))
           AND (lower(u.user_name) LIKE ? ESCAPE '\\' OR lower(COALESCE(p.display_name, '')) LIKE ? ESCAPE '\\'
                OR lower(COALESCE(p.display_name, '')) LIKE ? ESCAPE '\\')
         ORDER BY display_name LIMIT 50`,
      )
      .all(like, like, `% ${like}`) as { user_name: string; display_name: string }[];
    return { users: rows.map((r) => ({ user_name: r.user_name, display_name: r.display_name, presence: this.presenceOf(r.user_name) })) };
  }

  #reachable(userName: string): boolean {
    const known = this.#db
      .prepare('SELECT 1 FROM installations WHERE user_name = ? COLLATE NOCASE AND revoked_at IS NULL UNION SELECT 1 FROM user_profiles WHERE user_name = ? AND messaging = 1')
      .get(userName, userName);
    const off = this.#db.prepare('SELECT 1 FROM user_profiles WHERE user_name = ? AND messaging = 0').get(userName);
    const denied = this.#db.prepare("SELECT 1 FROM deny_list WHERE kind = 'user' AND lower(value) = lower(?)").get(userName);
    return Boolean(known) && !off && !denied;
  }

  #canonical(userName: string): string {
    return (
      (this.#db.prepare('SELECT user_name FROM user_profiles WHERE user_name = ?').get(userName) as { user_name: string } | undefined)?.user_name ??
      (this.#db.prepare('SELECT user_name FROM installations WHERE user_name = ? COLLATE NOCASE LIMIT 1').get(userName) as { user_name: string } | undefined)?.user_name ??
      userName
    );
  }

  // ---- Chats ----------------------------------------------------------------------------------------------------

  #record(user: User, action: string, details: Record<string, unknown>) {
    this.#audit.write({ category: 'messaging', action, outcome: 'ok', subject: user.subject, userName: user.userName, details });
  }

  #chat(user: User, id: string): ChatRow {
    const c = this.#db
      .prepare('SELECT c.* FROM chats c JOIN chat_members m ON m.chat = c.id WHERE c.id = ? AND m.user_name = ?')
      .get(id, user.userName) as ChatRow | undefined;
    if (!c) throw new ApiError(404, 'NOT_FOUND', 'No such Chat.');
    return c;
  }

  #members(chat: string): string[] {
    return (this.#db.prepare('SELECT user_name FROM chat_members WHERE chat = ? ORDER BY joined_at, user_name').all(chat) as { user_name: string }[]).map((r) => r.user_name);
  }

  #view(user: User, c: ChatRow) {
    const read = (this.#db.prepare('SELECT read_seq FROM chat_members WHERE chat = ? AND user_name = ?').get(c.id, user.userName) as { read_seq: number } | undefined)?.read_seq ?? 0;
    const last = this.#db.prepare('SELECT * FROM chat_messages WHERE chat = ? ORDER BY seq DESC LIMIT 1').get(c.id) as MessageRow | undefined;
    const unread = (this.#db.prepare('SELECT COUNT(*) AS n FROM chat_messages WHERE chat = ? AND seq > ? AND sender != ?').get(c.id, read, user.userName) as { n: number }).n;
    return {
      id: c.id,
      kind: c.kind,
      name: c.name,
      members: this.#members(c.id).map((u) => ({ user_name: u, display_name: this.#displayName(u), presence: this.presenceOf(u) })),
      last_seq: c.last_seq,
      read_seq: read,
      unread: Number(unread),
      last_message: last ? this.#message(last) : null,
      created_at: c.created_at,
    };
  }

  #message(m: MessageRow) {
    return { id: m.id, chat: m.chat, seq: m.seq, sender: m.sender, sender_name: this.#displayName(m.sender), text: m.text, at: m.at };
  }

  chats(user: User) {
    this.enter(user);
    const rows = this.#db
      .prepare('SELECT c.* FROM chats c JOIN chat_members m ON m.chat = c.id WHERE m.user_name = ? ORDER BY c.last_seq = 0, c.created_at DESC')
      .all(user.userName) as unknown as ChatRow[];
    const views = rows.map((c) => this.#view(user, c));
    views.sort((a, b) => (b.last_message?.at ?? b.created_at).localeCompare(a.last_message?.at ?? a.created_at));
    return { chats: views };
  }

  #changed(c: ChatRow, extraUsers: string[] = []) {
    for (const u of new Set([...this.#members(c.id), ...extraUsers])) this.#push.send('chat-changed', { user: u }, { chat: c.id }, 3600);
  }

  createChat(user: User, body: Record<string, unknown>) {
    this.enter(user);
    const now = nowIso();
    if (body.kind === 'direct') {
      if (typeof body.user !== 'string' || !body.user.trim()) throw new ApiError(400, 'INVALID_REQUEST', 'user is required.');
      const other = this.#canonical(body.user.trim());
      if (lower(other) === lower(user.userName)) throw new ApiError(400, 'INVALID_REQUEST', 'A direct Chat needs another user.');
      if (!this.#reachable(other)) throw new ApiError(404, 'USER_UNAVAILABLE', `${other} cannot be messaged.`);
      const key = [lower(user.userName), lower(other)].sort().join('\n');
      const existing = this.#db.prepare('SELECT * FROM chats WHERE pair_key = ?').get(key) as ChatRow | undefined;
      if (existing) {
        // A user who left a direct Chat gets back into it.
        this.#db.prepare('INSERT OR IGNORE INTO chat_members (chat, user_name, joined_at) VALUES (?, ?, ?)').run(existing.id, user.userName, now);
        this.#db.prepare('INSERT OR IGNORE INTO chat_members (chat, user_name, joined_at) VALUES (?, ?, ?)').run(existing.id, other, now);
        return this.#view(user, existing);
      }
      const id = randomUUID();
      tx(this.#db, () => {
        this.#db.prepare("INSERT INTO chats (id, kind, name, pair_key, created_by, created_at) VALUES (?, 'direct', NULL, ?, ?, ?)").run(id, key, user.userName, now);
        for (const u of [user.userName, other]) this.#db.prepare('INSERT INTO chat_members (chat, user_name, joined_at) VALUES (?, ?, ?)').run(id, u, now);
      });
      this.#record(user, 'messaging.chat.create', { chat: id, kind: 'direct', members: [user.userName, other] });
      const c = this.#db.prepare('SELECT * FROM chats WHERE id = ?').get(id) as unknown as ChatRow;
      this.#changed(c);
      return this.#view(user, c);
    }
    if (body.kind === 'team') {
      const name = this.#name(body.name);
      const others = this.#users(body.members, user);
      if (others.length < 1) throw new ApiError(400, 'INVALID_REQUEST', 'A team Chat needs at least one other member.');
      if (others.length + 1 > MAX_TEAM_MEMBERS) throw new ApiError(400, 'INVALID_REQUEST', `A team Chat has at most ${MAX_TEAM_MEMBERS} members.`);
      const id = randomUUID();
      tx(this.#db, () => {
        this.#db.prepare("INSERT INTO chats (id, kind, name, pair_key, created_by, created_at) VALUES (?, 'team', ?, NULL, ?, ?)").run(id, name, user.userName, now);
        for (const u of [user.userName, ...others]) this.#db.prepare('INSERT INTO chat_members (chat, user_name, joined_at) VALUES (?, ?, ?)').run(id, u, now);
      });
      this.#record(user, 'messaging.chat.create', { chat: id, kind: 'team', name, members: [user.userName, ...others] });
      const c = this.#db.prepare('SELECT * FROM chats WHERE id = ?').get(id) as unknown as ChatRow;
      this.#changed(c);
      return this.#view(user, c);
    }
    throw new ApiError(400, 'INVALID_REQUEST', 'kind must be direct or team.');
  }

  #name(v: unknown): string {
    if (typeof v !== 'string' || !v.trim() || v.trim().length > 100) throw new ApiError(400, 'INVALID_REQUEST', 'name must be 1-100 characters.');
    return v.trim();
  }

  #users(v: unknown, user: User): string[] {
    if (!Array.isArray(v) || !v.every((x) => typeof x === 'string')) throw new ApiError(400, 'INVALID_REQUEST', 'members must be a list of user names.');
    const out = [...new Map((v as string[]).map((x) => this.#canonical(x.trim())).filter((x) => x && lower(x) !== lower(user.userName)).map((x) => [lower(x), x])).values()];
    const unknown = out.filter((u) => !this.#reachable(u));
    if (unknown.length) throw new ApiError(404, 'USER_UNAVAILABLE', `These users cannot be messaged: ${unknown.join(', ')}.`, { users: unknown });
    return out;
  }

  renameChat(user: User, id: string, body: Record<string, unknown>) {
    this.enter(user);
    const c = this.#chat(user, id);
    if (c.kind !== 'team') throw new ApiError(400, 'INVALID_REQUEST', 'Only a team Chat has a name.');
    const name = this.#name(body.name);
    this.#db.prepare('UPDATE chats SET name = ? WHERE id = ?').run(name, id);
    this.#record(user, 'messaging.chat.rename', { chat: id, before: c.name, after: name });
    this.#changed(c);
    return this.#view(user, this.#chat(user, id));
  }

  addMembers(user: User, id: string, body: Record<string, unknown>) {
    this.enter(user);
    const c = this.#chat(user, id);
    if (c.kind !== 'team') throw new ApiError(400, 'INVALID_REQUEST', 'Members can be added to a team Chat only.');
    const current = new Set(this.#members(id).map(lower));
    const add = this.#users(body.users, user).filter((u) => !current.has(lower(u)));
    if (current.size + add.length > MAX_TEAM_MEMBERS) throw new ApiError(400, 'INVALID_REQUEST', `A team Chat has at most ${MAX_TEAM_MEMBERS} members.`);
    const now = nowIso();
    for (const u of add) this.#db.prepare('INSERT INTO chat_members (chat, user_name, joined_at) VALUES (?, ?, ?)').run(id, u, now);
    if (add.length) {
      this.#record(user, 'messaging.chat.members.add', { chat: id, added: add });
      this.#changed(c);
    }
    return this.#view(user, c);
  }

  leave(user: User, id: string) {
    this.enter(user);
    const c = this.#chat(user, id);
    this.#db.prepare('DELETE FROM chat_members WHERE chat = ? AND user_name = ?').run(id, user.userName);
    this.#record(user, 'messaging.chat.leave', { chat: id });
    this.#changed(c, [user.userName]);
    if (!this.#members(id).length) this.#db.prepare('DELETE FROM chats WHERE id = ?').run(id);
    return { left: id };
  }

  // ---- messages -------------------------------------------------------------------------------------------------

  send(user: User, id: string, body: Record<string, unknown>) {
    this.enter(user);
    const c = this.#chat(user, id);
    const mid = body.id;
    if (typeof mid !== 'string' || !MESSAGE_ID.test(mid)) throw new ApiError(400, 'INVALID_REQUEST', 'id must be 8-64 letters, digits, dashes or underscores.');
    const text = body.text;
    if (typeof text !== 'string' || !text.trim()) throw new ApiError(400, 'INVALID_REQUEST', 'text is required.');
    if (Buffer.byteLength(text, 'utf8') > this.#cfg.messaging.maxMessageBytes) throw new ApiError(413, 'TOO_LARGE', 'The message is longer than 8 KB.');
    const existing = this.#db.prepare('SELECT * FROM chat_messages WHERE chat = ? AND id = ?').get(id, mid) as MessageRow | undefined;
    if (existing) {
      if (lower(existing.sender) !== lower(user.userName)) throw new ApiError(409, 'CONFLICT', 'The message id is taken.');
      return this.#message(existing);
    }
    const at = nowIso();
    const seq = tx(this.#db, () => {
      const { last_seq } = this.#db.prepare('UPDATE chats SET last_seq = last_seq + 1 WHERE id = ? RETURNING last_seq').get(id) as { last_seq: number };
      this.#db.prepare('INSERT INTO chat_messages (chat, seq, id, sender, text, at) VALUES (?, ?, ?, ?, ?, ?)').run(id, last_seq, mid, user.userName, text, at);
      this.#db.prepare('UPDATE chat_members SET read_seq = ? WHERE chat = ? AND user_name = ?').run(last_seq, id, user.userName);
      return last_seq;
    });
    const m = this.#message({ chat: id, seq, id: mid, sender: user.userName, text, at });
    // Every member's installations, the sender's other laptops included; clients drop a message they have by id.
    for (const u of this.#members(c.id)) this.#push.send('chat-message', { user: u }, { chat: id, message: m }, 7 * 86400);
    return m;
  }

  messages(user: User, id: string, after: number, limit: number) {
    this.enter(user);
    this.#chat(user, id);
    const rows = this.#db
      .prepare('SELECT * FROM chat_messages WHERE chat = ? AND seq > ? ORDER BY seq LIMIT ?')
      .all(id, Math.max(0, after), Math.min(500, Math.max(1, limit))) as unknown as MessageRow[];
    return { messages: rows.map((m) => this.#message(m)) };
  }

  markRead(user: User, id: string, body: Record<string, unknown>) {
    this.enter(user);
    const c = this.#chat(user, id);
    if (!Number.isInteger(body.seq) || (body.seq as number) < 0) throw new ApiError(400, 'INVALID_REQUEST', 'seq must be a non-negative integer.');
    const seq = Math.min(body.seq as number, c.last_seq);
    this.#db.prepare('UPDATE chat_members SET read_seq = MAX(read_seq, ?) WHERE chat = ? AND user_name = ?').run(seq, id, user.userName);
    return { read_seq: (this.#db.prepare('SELECT read_seq FROM chat_members WHERE chat = ? AND user_name = ?').get(id, user.userName) as { read_seq: number }).read_seq };
  }

  /** Message Retention: deletes messages older than the retention period and Chats without members. */
  purge() {
    const cutoff = new Date(Date.now() - this.#cfg.messaging.retentionDays * 86400_000).toISOString();
    this.#db.prepare('DELETE FROM chat_messages WHERE at < ?').run(cutoff);
    this.#db.prepare('DELETE FROM chats WHERE NOT EXISTS (SELECT 1 FROM chat_members m WHERE m.chat = chats.id)').run();
  }

  /** Counts for the admin console; never content. */
  counts() {
    const n = (sql: string) => Number((this.#db.prepare(sql).get() as { n: number }).n);
    return {
      chats: n('SELECT COUNT(*) AS n FROM chats'),
      direct_chats: n("SELECT COUNT(*) AS n FROM chats WHERE kind = 'direct'"),
      team_chats: n("SELECT COUNT(*) AS n FROM chats WHERE kind = 'team'"),
      messages: n('SELECT COUNT(*) AS n FROM chat_messages'),
      messages_last_day: Number((this.#db.prepare('SELECT COUNT(*) AS n FROM chat_messages WHERE at >= ?').get(new Date(Date.now() - 86400_000).toISOString()) as { n: number }).n),
    };
  }
}

const wrap =
  (fn: (req: Request) => unknown) =>
  async (req: Request, res: Response, next: NextFunction) => {
    try {
      res.json(await fn(req));
    } catch (err) {
      next(err);
    }
  };
const body = (req: Request) => (req.body && typeof req.body === 'object' ? (req.body as Record<string, unknown>) : {});

/** Client side, for signed-in users: /api/v1/messaging. */
export function messagingRoutes(m: Messaging): Router {
  const r = Router();
  r.get('/directory', wrap((req) => m.directory(req.user!, str(req.query.q))));
  r.put('/presence', wrap((req) => m.setPresence(req.user!, req.header('x-skowork-installation') ?? undefined, body(req))));
  r.get('/chats', wrap((req) => m.chats(req.user!)));
  r.post('/chats', wrap((req) => m.createChat(req.user!, body(req))));
  r.patch('/chats/:id', wrap((req) => m.renameChat(req.user!, String(req.params.id), body(req))));
  r.post('/chats/:id/members', wrap((req) => m.addMembers(req.user!, String(req.params.id), body(req))));
  r.delete('/chats/:id/members/me', wrap((req) => m.leave(req.user!, String(req.params.id))));
  r.get('/chats/:id/messages', wrap((req) => m.messages(req.user!, String(req.params.id), Number(req.query.after ?? 0) || 0, Number(req.query.limit ?? 200) || 200)));
  r.post('/chats/:id/messages', wrap((req) => m.send(req.user!, String(req.params.id), body(req))));
  r.post('/chats/:id/read', wrap((req) => m.markRead(req.user!, String(req.params.id), body(req))));
  return r;
}
