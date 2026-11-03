// control-plane/fleet-registry push delivery (design D8/D10): a per-installation outbox in SQLite, delivered in id
// order over one WebSocket per installation, with ack and replay, auth-renew, and the close codes 4401 (token
// expired), 4403 (revoked) and 4409 (replaced by a newer connection).
import type { IncomingMessage } from 'node:http';
import { WebSocket, WebSocketServer } from 'ws';
import type { Config } from '../config.ts';
import type { Db } from '../db.ts';
import { nowIso, tx } from '../db.ts';
import type { Audit } from '../identity/audit.ts';
import { bearer, type IdentityService } from '../identity/identity.ts';

export const PUSH_TYPES = [
  'revocation',
  'minimum-version',
  'policy-changed',
  'skills-changed',
  'skill-revoked',
  'remote-task',
  'remote-task-claimed',
  'remote-task-cancelled',
  'chat-message',
  'chat-changed',
  'presence-changed',
] as const;
export type PushType = (typeof PUSH_TYPES)[number];

export const REMOTE_TASK_CLIENT_TYPES = ['remote-task-claim', 'remote-task-accept', 'remote-task-decline', 'remote-task-status'];

export const CLOSE_TOKEN_EXPIRED = 4401;
export const CLOSE_REVOKED = 4403;
export const CLOSE_REPLACED = 4409;

export type PushTarget = { installation_id: string } | { user: string } | { group: string } | { everyone: true };

export interface ClientTaskMessage {
  installationId: string;
  subject: string;
  userName: string;
  type: string;
  ref: number;
  payload: unknown;
}

/**
 * Handles remote-task client messages (control-plane/task-dispatch). The returned object is sent back as
 * {type: "reply", ref, payload}; throw TaskRefused to answer with an error frame instead (design D7).
 */
export type TaskSink = (m: ClientTaskMessage) => Promise<object | void> | object | void;
export class TaskRefused extends Error {
  readonly code: string;
  constructor(code: string, message: string) {
    super(message);
    this.code = code;
  }
}

interface Row {
  id: number;
  type: string;
  target: string;
  payload: string;
  issued_at: string;
  expires_at: string;
}

class Connection {
  readonly ws: WebSocket;
  readonly installationId: string;
  readonly subject: string;
  readonly userName: string;
  readonly ip: string;
  tokenExpiresAt: number;
  cursor: number;
  closing = false;
  delivering = false;
  again = false;

  constructor(ws: WebSocket, installationId: string, subject: string, userName: string, ip: string, tokenExpiresAt: number, lastAck: number) {
    this.ws = ws;
    this.installationId = installationId;
    this.subject = subject;
    this.userName = userName;
    this.ip = ip;
    this.tokenExpiresAt = tokenExpiresAt;
    this.cursor = Math.max(0, lastAck);
  }

  close(code: number, reason: string): void {
    if (this.closing) return;
    this.closing = true;
    try {
      this.ws.close(code, reason);
    } catch {
      /* already gone */
    }
    // Do not wait forever for the peer's close frame.
    setTimeout(() => this.ws.terminate(), 3000).unref();
  }
}

export class PushHub {
  readonly #cfg: Config;
  readonly #db: Db;
  readonly #audit: Audit;
  readonly #identity: IdentityService;
  readonly #connections = new Map<string, Connection>();
  readonly #wss: WebSocketServer;
  readonly #timer: NodeJS.Timeout;
  /** Called when a user's first push connection opens or last one closes (control-plane/user-messaging Presence). */
  onUserConnection: (userName: string) => void = () => {};
  taskSink: TaskSink = () => {
    throw new TaskRefused('NOT_AVAILABLE', 'Remote tasks are not available yet.');
  };

  constructor(cfg: Config, db: Db, audit: Audit, identity: IdentityService) {
    this.#cfg = cfg;
    this.#db = db;
    this.#audit = audit;
    this.#identity = identity;
    this.#wss = new WebSocketServer({ noServer: true, maxPayload: cfg.fleet.maxClientMessageBytes });
    this.#timer = setInterval(() => this.#checkTokens(), cfg.fleet.tokenCheckIntervalMs);
    this.#timer.unref();
  }

  get connectedCount(): number {
    return this.#connections.size;
  }

  isConnected(installationId: string): boolean {
    return this.#connections.has(installationId);
  }

  /** The installations of a user with an open push connection. */
  connectedInstallations(userName: string): string[] {
    const u = userName.toLowerCase();
    return [...this.#connections.values()].filter((c) => c.userName.toLowerCase() === u).map((c) => c.installationId);
  }

  close(): void {
    clearInterval(this.#timer);
    for (const c of this.#connections.values()) c.ws.terminate();
    this.#connections.clear();
    this.#wss.close();
  }

  // ---- handshake --------------------------------------------------------------------------------

  async handleUpgrade(req: IncomingMessage, socket: import('node:stream').Duplex, head: Buffer): Promise<void> {
    const reject = (status: number, code: string, message: string) => {
      const body = JSON.stringify({ error: { code, message } });
      socket.end(
        `HTTP/1.1 ${status} ${status === 401 ? 'Unauthorized' : status === 403 ? 'Forbidden' : status === 404 ? 'Not Found' : 'Bad Request'}\r\n` +
          `Content-Type: application/json\r\nContent-Length: ${Buffer.byteLength(body)}\r\nConnection: close\r\n\r\n${body}`,
      );
    };
    const url = new URL(req.url ?? '/', 'http://localhost');
    const installationId = url.searchParams.get('installation_id') ?? '';
    const lastAckRaw = url.searchParams.get('last_ack');
    const lastAck = lastAckRaw === null || lastAckRaw === '' ? 0 : Number(lastAckRaw);
    if (!/^[0-9a-f-]{36}$/i.test(installationId)) return reject(400, 'INVALID_REQUEST', 'installation_id must be a UUID.');
    if (!Number.isInteger(lastAck)) return reject(400, 'INVALID_REQUEST', 'last_ack must be an integer.');
    const token = bearer(req);
    const ip = req.socket.remoteAddress ?? '';
    if (!token) return reject(401, 'UNAUTHORIZED', 'A bearer access token is required.');
    const auth = await this.#identity.authenticate(token, { installationId, ip, path: url.pathname });
    if (!auth.ok) return reject(auth.status, auth.code, auth.reason);
    const inst = this.#db.prepare('SELECT subject, revoked_at FROM installations WHERE id = ?').get(installationId.toLowerCase()) as
      | { subject: string; revoked_at: string | null }
      | undefined;
    if (!inst) return reject(404, 'INSTALLATION_UNKNOWN', 'The installation is not registered; register first.');
    if (inst.subject !== auth.user.subject) {
      this.#audit.write({
        category: 'security', action: 'fleet.push-handshake-refused', outcome: 'refused', subject: auth.user.subject,
        userName: auth.user.userName, installationId, sourceIp: ip, reason: 'installation belongs to a different user',
      });
      return reject(403, 'FORBIDDEN', 'The installation belongs to a different user.');
    }
    this.#db.prepare('UPDATE installations SET last_seen = ? WHERE id = ?').run(nowIso(), installationId.toLowerCase());

    this.#wss.handleUpgrade(req, socket, head, (ws) => {
      const conn = new Connection(ws, installationId.toLowerCase(), auth.user.subject, auth.user.userName, ip, auth.user.tokenExpiresAt, lastAck);
      if (inst.revoked_at) {
        conn.close(CLOSE_REVOKED, 'installation revoked');
        return;
      }
      const previous = this.#connections.get(conn.installationId);
      this.#connections.set(conn.installationId, conn);
      if (previous) previous.close(CLOSE_REPLACED, 'replaced by a newer connection');
      ws.on('message', (data, isBinary) => void this.#onMessage(conn, data as Buffer, isBinary));
      ws.on('close', () => {
        if (this.#connections.get(conn.installationId) === conn) {
          this.#connections.delete(conn.installationId);
          this.#notifyUser(conn.userName);
        }
      });
      ws.on('error', () => ws.terminate());
      this.#deliver(conn);
      if (!previous) this.#notifyUser(conn.userName);
    });
  }

  #notifyUser(userName: string): void {
    try {
      this.onUserConnection(userName);
    } catch {
      /* presence is best effort */
    }
  }

  // ---- server -> client ---------------------------------------------------------------------------

  /** Stores a message for every installation the target names and delivers it to the connected ones. */
  send(type: PushType, target: PushTarget, payload: unknown, ttlS?: number): { recipients: number } {
    const ids = this.#resolve(target);
    const issued = new Date();
    const expires = new Date(issued.getTime() + Math.min(ttlS ?? this.#cfg.fleet.pushRetentionS, this.#cfg.fleet.pushRetentionS) * 1000);
    const targetJson = JSON.stringify(target);
    const payloadJson = JSON.stringify(payload ?? {});
    tx(this.#db, () => {
      const seq = this.#db.prepare(
        `INSERT INTO push_sequences (installation_id, last_id) VALUES (?, 1)
         ON CONFLICT (installation_id) DO UPDATE SET last_id = last_id + 1 RETURNING last_id`,
      );
      const ins = this.#db.prepare(
        'INSERT INTO push_messages (installation_id, id, type, target, payload, issued_at, expires_at) VALUES (?, ?, ?, ?, ?, ?, ?)',
      );
      for (const id of ids) {
        const { last_id } = seq.get(id) as { last_id: number };
        ins.run(id, last_id, type, targetJson, payloadJson, issued.toISOString(), expires.toISOString());
      }
    });
    for (const id of ids) {
      const c = this.#connections.get(id);
      if (c) this.#deliver(c);
    }
    return { recipients: ids.length };
  }

  #resolve(target: PushTarget): string[] {
    if ('installation_id' in target) {
      const row = this.#db.prepare('SELECT id FROM installations WHERE id = ?').get(target.installation_id.toLowerCase()) as { id: string } | undefined;
      return row ? [row.id] : [];
    }
    if ('user' in target) {
      return (this.#db.prepare('SELECT id FROM installations WHERE user_name = ? COLLATE NOCASE').all(target.user) as { id: string }[]).map((r) => r.id);
    }
    if ('everyone' in target) {
      return (this.#db.prepare('SELECT id FROM installations WHERE revoked_at IS NULL').all() as { id: string }[]).map((r) => r.id);
    }
    return (
      this.#db
        .prepare('SELECT i.id AS id FROM installations i WHERE EXISTS (SELECT 1 FROM json_each(i.groups) g WHERE g.value = ?)')
        .all(target.group) as { id: string }[]
    ).map((r) => r.id);
  }

  #deliver(c: Connection): void {
    if (c.delivering) {
      c.again = true;
      return;
    }
    c.delivering = true;
    try {
      do {
        c.again = false;
        for (;;) {
          if (c.closing || c.ws.readyState !== WebSocket.OPEN) return;
          const now = nowIso();
          const batch = this.#db
            .prepare(
              `SELECT id, type, target, payload, issued_at, expires_at FROM push_messages
               WHERE installation_id = ? AND id > ? AND acked_at IS NULL AND expires_at > ? ORDER BY id LIMIT 100`,
            )
            .all(c.installationId, c.cursor, now) as unknown as Row[];
          if (batch.length === 0) break;
          // Mark as delivered before sending, so an ack that races the send is always for a delivered message.
          this.#db
            .prepare(`UPDATE push_messages SET delivered_at = COALESCE(delivered_at, ?) WHERE installation_id = ? AND id BETWEEN ? AND ?`)
            .run(now, c.installationId, batch[0]!.id, batch[batch.length - 1]!.id);
          let revocation = false;
          for (const m of batch) {
            c.ws.send(
              JSON.stringify({
                id: m.id,
                type: m.type,
                target: JSON.parse(m.target),
                issued_at: m.issued_at,
                expires_at: m.expires_at,
                payload: JSON.parse(m.payload),
              }),
            );
            c.cursor = m.id;
            revocation ||= m.type === 'revocation';
          }
          if (revocation && this.#isRevoked(c)) {
            c.close(CLOSE_REVOKED, 'installation revoked');
            return;
          }
        }
      } while (c.again);
    } finally {
      c.delivering = false;
    }
  }

  #isRevoked(c: Connection): boolean {
    const inst = this.#db.prepare('SELECT revoked_at FROM installations WHERE id = ?').get(c.installationId) as { revoked_at: string | null } | undefined;
    return Boolean(inst?.revoked_at) || Boolean(this.#identity.findDenied(c.userName, c.installationId));
  }

  /** Closes connections whose last presented token expired more than the grace period ago (design D10: 4401). */
  #checkTokens(): void {
    const now = Date.now() / 1000;
    for (const c of this.#connections.values()) {
      if (now > c.tokenExpiresAt + this.#cfg.fleet.tokenExpiryGraceS) c.close(CLOSE_TOKEN_EXPIRED, 'access token expired');
    }
  }

  /** Closes every connection of a user or installation with 4403 (after the revocation message has been sent). */
  closeRevoked(target: { user: string } | { installation_id: string }): void {
    for (const c of this.#connections.values()) {
      const match = 'user' in target ? c.userName.toLowerCase() === target.user.toLowerCase() : c.installationId === target.installation_id.toLowerCase();
      if (match) setTimeout(() => c.close(CLOSE_REVOKED, 'access revoked'), 200).unref();
    }
  }

  // ---- client -> server ---------------------------------------------------------------------------

  async #onMessage(c: Connection, data: Buffer, isBinary: boolean): Promise<void> {
    if (isBinary) return this.#reject(c, null, 'BAD_MESSAGE', 'Only text messages are accepted.', false);
    let msg: { type?: unknown; ref?: unknown; payload?: unknown };
    try {
      msg = JSON.parse(data.toString('utf8'));
    } catch {
      return this.#reject(c, null, 'BAD_MESSAGE', 'The message is not valid JSON.', false);
    }
    const type = typeof msg?.type === 'string' ? msg.type : null;
    const ref = Number.isInteger(msg?.ref) ? (msg.ref as number) : null;
    if (type === 'auth-renew') return this.#authRenew(c, ref, msg.payload);
    if (type === 'ack') {
      if (ref === null) return this.#reject(c, null, 'BAD_MESSAGE', 'ack requires ref.', false);
      const r = this.#db
        .prepare(`UPDATE push_messages SET acked_at = COALESCE(acked_at, ?) WHERE installation_id = ? AND id = ? AND delivered_at IS NOT NULL`)
        .run(nowIso(), c.installationId, ref);
      if (r.changes === 0) return this.#notDelivered(c, type, ref);
      return;
    }
    if (type && REMOTE_TASK_CLIENT_TYPES.includes(type)) {
      if (ref === null) return this.#reject(c, null, 'BAD_MESSAGE', `${type} requires ref.`, false);
      const push = this.#db
        .prepare('SELECT type FROM push_messages WHERE installation_id = ? AND id = ? AND delivered_at IS NOT NULL')
        .get(c.installationId, ref) as { type: string } | undefined;
      if (push?.type !== 'remote-task') return this.#notDelivered(c, type, ref);
      try {
        const reply = await this.taskSink({ installationId: c.installationId, subject: c.subject, userName: c.userName, type, ref, payload: msg.payload });
        if (reply && c.ws.readyState === WebSocket.OPEN) c.ws.send(JSON.stringify({ type: 'reply', ref, payload: { message: type, ...reply } }));
      } catch (err) {
        if (err instanceof TaskRefused) return this.#sendError(c, ref, err.code, err.message, type);
        return this.#reject(c, ref, 'INTERNAL', 'The message could not be processed.', false);
      }
      return;
    }
    return this.#reject(c, ref, 'UNKNOWN_TYPE', `Message type '${type}' is not accepted.`, false);
  }

  async #authRenew(c: Connection, ref: number | null, payload: unknown): Promise<void> {
    const token = typeof (payload as { access_token?: unknown })?.access_token === 'string' ? (payload as { access_token: string }).access_token : '';
    if (!token) return this.#reject(c, ref, 'BAD_MESSAGE', 'auth-renew requires payload.access_token.', false);
    const r = await this.#identity.authenticate(token, { installationId: c.installationId, ip: c.ip, path: 'push:auth-renew' });
    if (!r.ok) {
      this.#sendError(c, ref, r.code, r.reason);
      if (r.status === 403) c.close(CLOSE_REVOKED, 'access revoked');
      return;
    }
    if (r.user.subject !== c.subject) {
      this.#audit.write({
        category: 'fleet', action: 'fleet.auth-renew-subject-mismatch', outcome: 'refused', subject: r.user.subject, userName: r.user.userName,
        installationId: c.installationId, sourceIp: c.ip, reason: 'auth-renew token belongs to a different subject than the connection',
      });
      this.#sendError(c, ref, 'SUBJECT_MISMATCH', 'The token belongs to a different user.');
      c.close(CLOSE_TOKEN_EXPIRED, 'subject mismatch');
      return;
    }
    if (r.user.tokenExpiresAt > c.tokenExpiresAt) c.tokenExpiresAt = r.user.tokenExpiresAt;
  }

  #notDelivered(c: Connection, type: string, ref: number): void {
    this.#reject(c, ref, 'NOT_DELIVERED', 'The referenced push message was not delivered to this installation.', true, {
      client_message: type,
      ref,
    });
  }

  #reject(c: Connection, ref: number | null, code: string, reason: string, security: boolean, details?: unknown): void {
    this.#audit.write({
      category: security ? 'security' : 'fleet',
      action: security ? 'fleet.forged-reference' : 'fleet.client-message-rejected',
      outcome: 'refused',
      subject: c.subject,
      userName: c.userName,
      installationId: c.installationId,
      sourceIp: c.ip,
      reason: `${code}: ${reason}`,
      details,
    });
    this.#sendError(c, ref, code, reason);
  }

  #sendError(c: Connection, ref: number | null, code: string, reason: string, message?: string): void {
    if (c.ws.readyState === WebSocket.OPEN) c.ws.send(JSON.stringify({ type: 'error', ref, payload: { code, reason, ...(message ? { message } : {}) } }));
  }

  /** The payload of a push message delivered to an installation (the remote-task a client message refers to). */
  pushPayload(installationId: string, id: number): unknown {
    const row = this.#db.prepare('SELECT payload FROM push_messages WHERE installation_id = ? AND id = ?').get(installationId, id) as { payload: string } | undefined;
    return row ? JSON.parse(row.payload) : undefined;
  }

  /** Whether a push of this type with payload.task_id was delivered to any installation. */
  taskDelivered(taskId: string): boolean {
    return Boolean(
      this.#db
        .prepare(`SELECT 1 FROM push_messages WHERE type = 'remote-task' AND delivered_at IS NOT NULL AND json_extract(payload, '$.task_id') = ? LIMIT 1`)
        .get(taskId),
    );
  }

  /** Deletes expired and acknowledged messages older than the retention (maintenance). */
  prune(): void {
    const cutoff = new Date(Date.now() - this.#cfg.fleet.pushRetentionS * 1000).toISOString();
    this.#db.prepare('DELETE FROM push_messages WHERE expires_at < ? OR (acked_at IS NOT NULL AND acked_at < ?)').run(nowIso(), cutoff);
  }
}
