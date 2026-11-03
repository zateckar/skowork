// control-plane/task-dispatch: Remote tasks requested by people and service principals for a user or a Group, authorized
// by the dispatch rules of control-plane/policy-service, delivered through the fleet outbox, claimed by one
// installation, tracked with their transitions, and audited (design D5, D7, D8).
import { randomUUID } from 'node:crypto';
import type { Db } from '../db.ts';
import { nowIso, tx } from '../db.ts';
import { ApiError } from '../http.ts';
import type { Audit } from '../identity/audit.ts';
import type { IdentityService, User } from '../identity/identity.ts';
import { TaskRefused, type ClientTaskMessage, type PushHub } from '../fleet/push.ts';
import type { Actor, DispatchIssuer, DispatchTarget, PolicyService } from '../policy/service.ts';

export interface TaskSettings {
  defaultPerHour: number; // per-issuer rate limit unless an administrator set one
  maxAttachmentBytes: number;
  defaultExpiryS: number;
  maxExpiryS: number;
  maxPromptChars: number;
  callbackTimeoutMs: number;
}

export const DEFAULT_TASK_SETTINGS: TaskSettings = {
  defaultPerHour: 100,
  maxAttachmentBytes: 10 * 1024 * 1024,
  defaultExpiryS: 24 * 3600,
  maxExpiryS: 7 * 24 * 3600,
  maxPromptChars: 100_000,
  callbackTimeoutMs: 10_000,
};

/** The APIs a service principal registration may list. */
export const SERVICE_APIS = ['tasks'] as const;

/** Who requests a task: a person or a registered service principal; `id` is the issuer id of design D7. */
export interface TaskIssuer {
  id: string;
  kind: 'user' | 'service';
  name: string;
  user?: User;
}

export function userIssuer(u: User): TaskIssuer {
  return { id: `user:${u.userName}`, kind: 'user', name: u.displayName || u.userName, user: u };
}
export function serviceIssuer(p: { name: string }): TaskIssuer {
  return { id: `service:${p.name}`, kind: 'service', name: p.name };
}

const OPEN = ['queued', 'delivered', 'claimed', 'awaiting-acceptance', 'accepted'];
const FINAL = ['succeeded', 'failed', 'declined', 'expired', 'cancelled'];
// remote-task-status states of the client (client/scheduler Occurrence states) to dispatch statuses (design D5).
const CLIENT_FINAL: Record<string, string> = { succeeded: 'succeeded', failed: 'failed', dead: 'failed', cancelled: 'cancelled' };

interface TaskRow {
  id: string;
  parent_id: string;
  issuer: string;
  issuer_name: string;
  user_name: string;
  prompt: string;
  skill: string | null;
  attachments: string;
  callback_url: string | null;
  status: string;
  claimed_by: string | null;
  result: string | null;
  created_at: string;
  expires_at: string;
}

interface Attachment {
  name: string;
  content_type: string;
  size: number;
  data: string; // base64
}

export class TaskDispatch {
  readonly #db: Db;
  readonly #audit: Audit;
  readonly #identity: IdentityService;
  readonly #policy: PolicyService;
  readonly #push: PushHub;
  readonly #s: TaskSettings;
  readonly #timer: NodeJS.Timeout;
  /** Callback deliveries in flight (tests wait for them). */
  readonly pending = new Set<Promise<void>>();

  constructor(db: Db, audit: Audit, identity: IdentityService, policy: PolicyService, push: PushHub, settings: TaskSettings = DEFAULT_TASK_SETTINGS) {
    this.#db = db;
    this.#audit = audit;
    this.#identity = identity;
    this.#policy = policy;
    this.#push = push;
    this.#s = settings;
    this.#timer = setInterval(() => this.expireDue(), 60_000);
    this.#timer.unref();
  }

  close(): void {
    clearInterval(this.#timer);
  }

  // ---- Task Request API ---------------------------------------------------------------------------------------------

  create(issuer: TaskIssuer, body: unknown, ip: string | null = null) {
    const b = (body ?? {}) as Record<string, unknown>;
    const refuse = (status: number, code: string, message: string, extra: Record<string, unknown> = {}): never => {
      this.#auditIssuer(issuer, 'tasks.request', 'refused', `${code}: ${message}`, { target: b.target ?? null }, ip);
      throw new ApiError(status, code, message, extra);
    };
    const prompt = typeof b.prompt === 'string' ? b.prompt.trim() : '';
    if (!prompt) refuse(400, 'INVALID_REQUEST', 'prompt is required.');
    if (prompt.length > this.#s.maxPromptChars) refuse(400, 'INVALID_REQUEST', `prompt is longer than ${this.#s.maxPromptChars} characters.`);
    const target = (b.target ?? {}) as Record<string, unknown>;
    const targetUser = typeof target.user === 'string' ? target.user.trim() : '';
    const targetGroup = typeof target.group === 'string' ? target.group.trim() : '';
    if (Boolean(targetUser) === Boolean(targetGroup)) refuse(400, 'INVALID_REQUEST', 'target needs exactly one of user or group.');
    const skill = b.skill === undefined || b.skill === null ? null : typeof b.skill === 'string' && b.skill.trim() ? b.skill.trim() : refuse(400, 'INVALID_REQUEST', 'skill must be a name.');
    const attachments = this.#attachments(b.attachments, refuse);
    const expiresIn = b.expires_in_s === undefined ? this.#s.defaultExpiryS : Number(b.expires_in_s);
    if (!Number.isInteger(expiresIn) || expiresIn < 60 || expiresIn > this.#s.maxExpiryS) {
      refuse(400, 'INVALID_REQUEST', `expires_in_s must be between 60 and ${this.#s.maxExpiryS} seconds.`);
    }
    let callback: string | null = null;
    if (b.callback_url !== undefined && b.callback_url !== null) {
      callback = typeof b.callback_url === 'string' ? b.callback_url : '';
      if (!this.callbackAllowed(callback)) refuse(400, 'CALLBACK_NOT_ALLOWED', 'The callback URL is not on the callback allowlist.');
    }

    // Dispatch Audit And Limits: requests in the last hour, per issuer.
    const limit = this.rateLimit(issuer.id);
    const hourAgo = new Date(Date.now() - 3600_000).toISOString();
    const used = (this.#db.prepare('SELECT COUNT(*) AS n, MIN(created_at) AS first FROM remote_task_parents WHERE issuer = ? AND created_at > ?').get(issuer.id, hourAgo) as {
      n: number;
      first: string | null;
    });
    if (used.n >= limit) {
      const retry = used.first ? Math.max(1, Math.ceil((Date.parse(used.first) + 3600_000 - Date.now()) / 1000)) : 3600;
      refuse(429, 'RATE_LIMIT', `The issuer may request ${limit} tasks per hour.`, { retry_after_s: retry });
    }

    // Targeting Authorization.
    const dIssuer: DispatchIssuer =
      issuer.kind === 'service'
        ? { kind: 'service', name: issuer.name }
        : { kind: 'user', userName: issuer.user!.userName, adminRoles: issuer.user!.adminRoles, groups: issuer.user!.groups };
    const dTarget: DispatchTarget = targetUser
      ? { kind: 'user', userName: targetUser, groups: this.#identity.groupsFor(targetUser, this.#identity.knownRolesFor(targetUser)) }
      : { kind: 'group', group: targetGroup };
    const decision = this.#policy.authorizeDispatch(dIssuer, dTarget);
    if (!decision.allowed) refuse(403, 'NOT_AUTHORIZED', 'No dispatch rule lets this issuer target ' + (targetUser ? `user ${targetUser}` : `Group ${targetGroup}`) + '.');

    // Group Fan-Out at request time.
    const members = targetUser ? [targetUser] : this.groupMembers(targetGroup);
    if (members.length === 0) refuse(400, 'NO_MEMBERS', `The Group ${targetGroup} has no members.`);

    const now = new Date();
    const created = now.toISOString();
    const expires = new Date(now.getTime() + expiresIn * 1000).toISOString();
    const parentId = `task_${randomUUID()}`;
    const rows = members.map((user) => ({ id: targetUser ? `${parentId}` + '.1' : `${parentId}.${randomUUID().slice(0, 8)}`, user }));
    tx(this.#db, () => {
      this.#db
        .prepare('INSERT INTO remote_task_parents (id, issuer, target_kind, target, created_at, expires_at) VALUES (?, ?, ?, ?, ?, ?)')
        .run(parentId, issuer.id, targetUser ? 'user' : 'group', targetUser || targetGroup, created, expires);
      const ins = this.#db.prepare(
        `INSERT INTO remote_tasks (id, parent_id, issuer, issuer_name, user_name, prompt, skill, attachments, callback_url, status, created_at, expires_at, updated_at)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 'queued', ?, ?, ?)`,
      );
      for (const r of rows) {
        ins.run(r.id, parentId, issuer.id, issuer.name, r.user, prompt, skill, JSON.stringify(attachments), callback, created, expires, created);
        this.#transition(r.id, 'queued', null, created);
      }
    });
    this.#auditIssuer(issuer, 'tasks.request', 'ok', null, { task: parentId, target: targetUser ? { user: targetUser } : { group: targetGroup }, members: rows.length, rule: decision.rule?.id ?? null }, ip);

    // Delivery Through Fleet Push: stored until it expires, to every installation of the user.
    const ttl = expiresIn;
    for (const r of rows) {
      const payload = {
        task_id: r.id,
        parent_id: parentId,
        issuer: { id: issuer.id, kind: issuer.kind, name: issuer.name },
        prompt,
        skill,
        attachments,
        expires_at: expires,
        provenance: { source_kind: 'remote', ref: issuer.id, untrusted: true },
      };
      const sent = this.#push.send('remote-task', { user: r.user }, payload, ttl);
      this.#audit.write({ category: 'tasks', action: 'tasks.deliver', outcome: 'ok', userName: r.user, details: { task: r.id, installations: sent.recipients } });
    }
    return { id: parentId, status: 'queued', expires_at: expires, members: rows.map((r) => ({ id: r.id, user: r.user, status: 'queued' })) };
  }

  #attachments(v: unknown, refuse: (s: number, c: string, m: string) => never): Attachment[] {
    if (v === undefined || v === null) return [];
    if (!Array.isArray(v)) refuse(400, 'INVALID_REQUEST', 'attachments must be a list.');
    let total = 0;
    return (v as unknown[]).map((a) => {
      const o = (a ?? {}) as Record<string, unknown>;
      const name = typeof o.name === 'string' ? o.name.trim() : '';
      const data = typeof o.data === 'string' ? o.data : '';
      if (!name || !data || !/^[A-Za-z0-9+/=\s]+$/.test(data)) refuse(400, 'INVALID_REQUEST', 'Each attachment needs a name and base64 data.');
      const size = Buffer.from(data, 'base64').length;
      total += size;
      if (total > this.#s.maxAttachmentBytes) refuse(413, 'TOO_LARGE', `Attachments may total at most ${this.#s.maxAttachmentBytes} bytes.`);
      return { name, content_type: typeof o.content_type === 'string' ? o.content_type : 'application/octet-stream', size, data: data.replace(/\s+/g, '') };
    });
  }

  /** The users of a Group: assigned in the control plane, or reported by their installations (design D8). */
  groupMembers(group: string): string[] {
    const rows = this.#db
      .prepare(
        `SELECT user_name FROM group_members WHERE workbench_group = ?
         UNION
         SELECT DISTINCT user_name FROM installations WHERE revoked_at IS NULL AND EXISTS (SELECT 1 FROM json_each(groups) g WHERE g.value = ?)`,
      )
      .all(group, group) as { user_name: string }[];
    const seen = new Map<string, string>();
    for (const r of rows) if (!seen.has(r.user_name.toLowerCase())) seen.set(r.user_name.toLowerCase(), r.user_name);
    return [...seen.values()].sort();
  }

  // ---- Task Status Tracking and Cancellation By Issuer ----------------------------------------------------------------

  status(issuer: TaskIssuer, id: string) {
    this.expireDue();
    const parent = this.#parent(issuer, id);
    const members = this.#db.prepare('SELECT * FROM remote_tasks WHERE parent_id = ? ORDER BY id').all(parent.id) as unknown as TaskRow[];
    for (const m of members) if (m.status === 'queued' && this.#push.taskDelivered(m.id)) this.#move(m, 'delivered', null);
    const fresh = this.#db.prepare('SELECT * FROM remote_tasks WHERE parent_id = ? ORDER BY id').all(parent.id) as unknown as TaskRow[];
    return {
      id: parent.id,
      issuer: parent.issuer,
      target: { [parent.target_kind]: parent.target },
      created_at: parent.created_at,
      expires_at: parent.expires_at,
      members: fresh.map((m) => ({
        id: m.id,
        user: m.user_name,
        status: m.status,
        transitions: this.#db.prepare('SELECT status, at FROM remote_task_transitions WHERE task_id = ? ORDER BY rowid').all(m.id),
        ...(m.result !== null ? { result: m.result } : {}),
      })),
    };
  }

  cancel(issuer: TaskIssuer, id: string, ip: string | null = null) {
    const parent = this.#parent(issuer, id);
    const members = this.#db
      .prepare(`SELECT * FROM remote_tasks WHERE parent_id = ? AND (id = ? OR ? = parent_id) ORDER BY id`)
      .all(parent.id, id, id) as unknown as TaskRow[];
    let cancelled = 0;
    for (const m of members) {
      if (FINAL.includes(m.status)) continue;
      this.#move(m, 'cancelled', null);
      this.#push.send('remote-task-cancelled', { user: m.user_name }, { task_id: m.id, reason: 'cancelled' });
      cancelled++;
      this.#finished({ ...m, status: 'cancelled' });
    }
    this.#auditIssuer(issuer, 'tasks.cancel', 'ok', null, { task: id, cancelled }, ip);
    return this.status(issuer, parent.id);
  }

  /** Expired before it ran: status expired, and remote-task-cancelled to every installation of the user. */
  expireDue(): void {
    const now = nowIso();
    const due = this.#db
      .prepare(`SELECT * FROM remote_tasks WHERE status IN (${OPEN.map(() => '?').join(',')}) AND expires_at <= ?`)
      .all(...OPEN, now) as unknown as TaskRow[];
    for (const m of due) {
      this.#move(m, 'expired', null);
      this.#push.send('remote-task-cancelled', { user: m.user_name }, { task_id: m.id, reason: 'expired' });
      this.#audit.write({ category: 'tasks', action: 'tasks.expire', outcome: 'ok', userName: m.user_name, details: { task: m.id } });
      this.#finished({ ...m, status: 'expired' });
    }
  }

  #parent(issuer: TaskIssuer, id: string) {
    const parentId = id.includes('.') ? id.slice(0, id.indexOf('.')) : id;
    const p = this.#db.prepare('SELECT * FROM remote_task_parents WHERE id = ?').get(parentId) as
      | { id: string; issuer: string; target_kind: string; target: string; created_at: string; expires_at: string }
      | undefined;
    // Another issuer's task does not exist for this one.
    if (!p || p.issuer !== issuer.id) throw new ApiError(404, 'NOT_FOUND', 'No such task.');
    return p;
  }

  #transition(taskId: string, status: string, installationId: string | null, at = nowIso()): void {
    this.#db.prepare('INSERT INTO remote_task_transitions (task_id, status, at, installation_id) VALUES (?, ?, ?, ?)').run(taskId, status, at, installationId);
  }

  #move(m: TaskRow, status: string, installationId: string | null, extra: { claimed_by?: string; result?: string | null } = {}): void {
    const at = nowIso();
    tx(this.#db, () => {
      this.#db
        .prepare('UPDATE remote_tasks SET status = ?, claimed_by = COALESCE(?, claimed_by), result = COALESCE(?, result), updated_at = ? WHERE id = ?')
        .run(status, extra.claimed_by ?? null, extra.result ?? null, at, m.id);
      this.#transition(m.id, status, installationId, at);
    });
  }

  // ---- Client Task Messages ---------------------------------------------------------------------------------------

  /** The fleet registry's TaskSink: one client message for the member task its remote-task push carried. */
  handleClient(m: ClientTaskMessage): object {
    const push = this.#push.pushPayload(m.installationId, m.ref) as { task_id?: string } | undefined;
    const row = push?.task_id ? (this.#db.prepare('SELECT * FROM remote_tasks WHERE id = ?').get(push.task_id) as unknown as TaskRow | undefined) : undefined;
    if (!row || row.user_name.toLowerCase() !== m.userName.toLowerCase()) throw new TaskRefused('NOT_FOUND', 'No such task for this user.');
    this.expireDue();
    let t = this.#row(row.id);
    const audit = (action: string, outcome: 'ok' | 'refused', reason: string | null, details: Record<string, unknown> = {}) =>
      this.#audit.write({
        category: outcome === 'refused' && action === 'tasks.not-claimant' ? 'security' : 'tasks', action, outcome, subject: m.subject, userName: m.userName,
        installationId: m.installationId, reason, details: { task: t.id, ...details },
      });

    if (m.type === 'remote-task-claim') {
      if (t.claimed_by === m.installationId && !['expired', 'cancelled'].includes(t.status)) return { task_id: t.id, status: t.status };
      if (t.status !== 'queued' && t.status !== 'delivered') {
        const why = t.status === 'cancelled' ? 'the issuer cancelled the task' : t.status === 'expired' ? 'the task expired' : t.claimed_by ? 'another installation claimed the task' : `the task is ${t.status}`;
        audit('tasks.claim', 'refused', why);
        throw new TaskRefused('CLAIM_REFUSED', `The claim is refused: ${why}.`);
      }
      // The claim is a conditional update: only the first installation wins.
      const won = tx(this.#db, () => {
        const r = this.#db
          .prepare(`UPDATE remote_tasks SET status = 'claimed', claimed_by = ?, updated_at = ? WHERE id = ? AND status IN ('queued', 'delivered')`)
          .run(m.installationId, nowIso(), t.id);
        if (r.changes === 0) return false;
        if (t.status === 'queued') this.#transition(t.id, 'delivered', m.installationId);
        this.#transition(t.id, 'claimed', m.installationId);
        return true;
      });
      if (!won) {
        audit('tasks.claim', 'refused', 'another installation claimed the task');
        throw new TaskRefused('CLAIM_REFUSED', 'The claim is refused: another installation claimed the task.');
      }
      t = this.#row(t.id);
      const trusted = this.#userList(t.user_name, 'scheduler.remote_trusted_issuers').includes(t.issuer);
      this.#move(t, trusted ? 'accepted' : 'awaiting-acceptance', m.installationId);
      audit('tasks.claim', 'ok', null, { trusted });
      const others = this.#db
        .prepare('SELECT id FROM installations WHERE user_name = ? COLLATE NOCASE AND id <> ?')
        .all(t.user_name, m.installationId) as { id: string }[];
      for (const o of others) this.#push.send('remote-task-claimed', { installation_id: o.id }, { task_id: t.id });
      return { task_id: t.id, status: trusted ? 'accepted' : 'awaiting-acceptance' };
    }

    // Everything else only from the installation that holds the claim.
    if (t.claimed_by !== m.installationId) {
      audit('tasks.not-claimant', 'refused', `${m.type} from an installation that does not hold the claim`);
      throw new TaskRefused('NOT_CLAIMANT', 'This installation does not hold the claim of the task.');
    }
    if (FINAL.includes(t.status)) return { task_id: t.id, status: t.status }; // already ended (cancelled, expired)

    if (m.type === 'remote-task-accept') {
      if (t.status === 'awaiting-acceptance' || t.status === 'claimed') this.#move(t, 'accepted', m.installationId);
      audit('tasks.accept', 'ok', null);
      return { task_id: t.id, status: this.#row(t.id).status };
    }
    if (m.type === 'remote-task-decline') {
      this.#move(t, 'declined', m.installationId);
      audit('tasks.decline', 'ok', null);
      this.#finished(this.#row(t.id));
      return { task_id: t.id, status: 'declined' };
    }
    // remote-task-status
    const p = (m.payload ?? {}) as { state?: unknown; result?: unknown };
    const state = typeof p.state === 'string' ? p.state : '';
    if (state === 'running') {
      if (t.status !== 'running') this.#move(t, 'running', m.installationId);
      return { task_id: t.id, status: 'running' };
    }
    const final = CLIENT_FINAL[state];
    if (!final) throw new TaskRefused('BAD_MESSAGE', 'state must be running, succeeded, failed, dead or cancelled.');
    // Result Sharing: content only when the user's policy lists the issuer.
    let result: string | null = null;
    if (typeof p.result === 'string' && p.result && this.#userList(t.user_name, 'scheduler.remote_share_results').includes(t.issuer)) {
      result = p.result;
      audit('tasks.result-disclosed', 'ok', null, { issuer: t.issuer, chars: result.length });
    }
    this.#move(t, final, m.installationId, { result });
    audit('tasks.status', 'ok', null, { state, status: final });
    this.#finished(this.#row(t.id));
    return { task_id: t.id, status: final };
  }

  #row(id: string): TaskRow {
    return this.#db.prepare('SELECT * FROM remote_tasks WHERE id = ?').get(id) as unknown as TaskRow;
  }

  #userList(userName: string, key: string): string[] {
    const groups = this.#identity.groupsFor(userName, this.#identity.knownRolesFor(userName));
    const v = this.#policy.compileFor(groups, userName).flat.get(key);
    return Array.isArray(v) ? (v as string[]) : [];
  }

  /** A final status reaches the issuer's callback, when the request named one (Result Sharing). */
  #finished(t: TaskRow): void {
    if (!t.callback_url || !FINAL.includes(t.status)) return;
    const url = t.callback_url;
    if (!this.callbackAllowed(url)) {
      this.#audit.write({ category: 'tasks', action: 'tasks.callback', outcome: 'refused', userName: t.user_name, reason: 'no longer on the callback allowlist', details: { task: t.id } });
      return;
    }
    const body = { task_id: t.id, parent_id: t.parent_id, status: t.status, ...(t.result !== null ? { result: t.result } : {}) };
    const p = (async () => {
      try {
        const res = await fetch(url, {
          method: 'POST',
          headers: { 'content-type': 'application/json' },
          body: JSON.stringify(body),
          signal: AbortSignal.timeout(this.#s.callbackTimeoutMs),
          redirect: 'manual',
        });
        this.#audit.write({
          category: 'tasks', action: 'tasks.callback', outcome: res.ok ? 'ok' : 'error', userName: t.user_name,
          reason: res.ok ? null : `answered ${res.status}`, details: { task: t.id, status: t.status, result: t.result !== null },
        });
      } catch (err) {
        this.#audit.write({ category: 'tasks', action: 'tasks.callback', outcome: 'error', userName: t.user_name, reason: (err as Error).message, details: { task: t.id } });
      }
    })();
    this.pending.add(p);
    void p.finally(() => this.pending.delete(p));
  }

  #auditIssuer(issuer: TaskIssuer, action: string, outcome: 'ok' | 'refused', reason: string | null, details: Record<string, unknown>, ip: string | null): void {
    this.#audit.write({
      category: 'tasks', action, outcome, subject: issuer.user?.subject ?? issuer.id, userName: issuer.user?.userName ?? null,
      sourceIp: ip, reason, details: { issuer: issuer.id, ...details },
    });
  }

  // ---- settings administered in the console (design D8) ----------------------------------------------------------

  rateLimit(issuer: string): number {
    const row = this.#db.prepare('SELECT per_hour FROM task_issuer_limits WHERE issuer = ?').get(issuer) as { per_hour: number } | undefined;
    return row?.per_hour ?? this.#s.defaultPerHour;
  }

  listLimits() {
    return { default_per_hour: this.#s.defaultPerHour, limits: this.#db.prepare('SELECT issuer, per_hour, updated_by, updated_at FROM task_issuer_limits ORDER BY issuer').all() };
  }

  setLimit(a: Actor, issuer: string, perHour: number) {
    if (!/^(user|service):.+/.test(issuer)) throw new ApiError(400, 'INVALID_REQUEST', 'issuer must be an issuer id such as service:ci or user:jnovak.');
    if (!Number.isInteger(perHour) || perHour < 0 || perHour > 100_000) throw new ApiError(400, 'INVALID_REQUEST', 'per_hour must be an integer from 0 to 100000.');
    const before = this.#db.prepare('SELECT per_hour FROM task_issuer_limits WHERE issuer = ?').get(issuer) ?? null;
    this.#db
      .prepare('INSERT INTO task_issuer_limits (issuer, per_hour, updated_by, updated_at) VALUES (?, ?, ?, ?) ON CONFLICT (issuer) DO UPDATE SET per_hour = excluded.per_hour, updated_by = excluded.updated_by, updated_at = excluded.updated_at')
      .run(issuer, perHour, a.userName, nowIso());
    this.#admin(a, 'tasks.limit.set', { issuer, before, after: { per_hour: perHour } });
    return { issuer, per_hour: perHour };
  }

  removeLimit(a: Actor, issuer: string) {
    const row = this.#db.prepare('DELETE FROM task_issuer_limits WHERE issuer = ? RETURNING issuer, per_hour').get(issuer);
    if (!row) throw new ApiError(404, 'NOT_FOUND', 'No limit is set for this issuer.');
    this.#admin(a, 'tasks.limit.remove', { before: row, after: null });
    return { removed: true };
  }

  listCallbacks() {
    return this.#db.prepare('SELECT prefix, added_by, added_at FROM task_callback_allowlist ORDER BY prefix').all();
  }

  addCallback(a: Actor, prefix: string) {
    let u: URL;
    try {
      u = new URL(prefix);
    } catch {
      throw new ApiError(400, 'INVALID_REQUEST', 'The callback prefix must be an absolute URL.');
    }
    if (u.protocol !== 'https:' && u.protocol !== 'http:') throw new ApiError(400, 'INVALID_REQUEST', 'Callbacks use http or https.');
    if (u.username || u.password || u.search || u.hash) throw new ApiError(400, 'INVALID_REQUEST', 'The prefix must not contain credentials, a query or a fragment.');
    const norm = u.toString();
    this.#db.prepare('INSERT OR IGNORE INTO task_callback_allowlist (prefix, added_by, added_at) VALUES (?, ?, ?)').run(norm, a.userName, nowIso());
    this.#admin(a, 'tasks.callback-allowlist.add', { before: null, after: { prefix: norm } });
    return { prefix: norm };
  }

  removeCallback(a: Actor, prefix: string) {
    const row = this.#db.prepare('DELETE FROM task_callback_allowlist WHERE prefix = ? RETURNING prefix').get(prefix);
    if (!row) throw new ApiError(404, 'NOT_FOUND', 'The prefix is not on the allowlist.');
    this.#admin(a, 'tasks.callback-allowlist.remove', { before: row, after: null });
    return { removed: true };
  }

  /** A URL starts with an allowlisted prefix: same scheme and host, and a path under the prefix's path. */
  callbackAllowed(url: string): boolean {
    let u: URL;
    try {
      u = new URL(url);
    } catch {
      return false;
    }
    if (u.username || u.password) return false;
    for (const { prefix } of this.listCallbacks() as { prefix: string }[]) {
      const p = new URL(prefix);
      if (u.protocol !== p.protocol || u.host !== p.host) continue;
      const base = p.pathname.endsWith('/') ? p.pathname : p.pathname + '/';
      if (u.pathname === p.pathname || u.pathname.startsWith(base)) return true;
    }
    return false;
  }

  listPrincipals() {
    return (this.#db.prepare('SELECT client_id, name, apis, registered_by, registered_at, retired_by, retired_at FROM service_principals ORDER BY name').all() as { apis: string }[]).map(
      (r) => ({ ...r, apis: JSON.parse(r.apis) as string[] }),
    );
  }

  registerPrincipal(a: Actor, body: unknown) {
    const b = (body ?? {}) as Record<string, unknown>;
    const clientId = typeof b.client_id === 'string' ? b.client_id.trim() : '';
    const name = typeof b.name === 'string' ? b.name.trim() : '';
    const apis = Array.isArray(b.apis) ? b.apis.filter((x): x is string => typeof x === 'string') : [];
    if (!clientId || !/^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/.test(name)) throw new ApiError(400, 'INVALID_REQUEST', 'client_id and a name (letters, digits, . _ -) are required.');
    const unknown = apis.filter((x) => !(SERVICE_APIS as readonly string[]).includes(x));
    if (apis.length === 0 || unknown.length) throw new ApiError(400, 'INVALID_REQUEST', `apis must list some of: ${SERVICE_APIS.join(', ')}.`);
    const existing = this.#db.prepare('SELECT client_id FROM service_principals WHERE client_id = ? OR name = ?').get(clientId, name);
    if (existing) throw new ApiError(409, 'EXISTS', 'A service principal with this client id or name is already registered.');
    this.#db
      .prepare('INSERT INTO service_principals (client_id, name, apis, registered_by, registered_at) VALUES (?, ?, ?, ?, ?)')
      .run(clientId, name, JSON.stringify(apis), a.userName, nowIso());
    this.#admin(a, 'identity.service-principal.register', { before: null, after: { client_id: clientId, name, apis } });
    return { client_id: clientId, name, apis };
  }

  retirePrincipal(a: Actor, clientId: string) {
    const r = this.#db.prepare('UPDATE service_principals SET retired_by = ?, retired_at = ? WHERE client_id = ? AND retired_at IS NULL RETURNING client_id, name').get(a.userName, nowIso(), clientId);
    if (!r) throw new ApiError(404, 'NOT_FOUND', 'No active service principal has this client id.');
    this.#admin(a, 'identity.service-principal.retire', { before: r, after: null });
    return { retired: true };
  }

  #admin(a: Actor, action: string, details: Record<string, unknown>): void {
    this.#audit.write({ category: 'admin', action, outcome: 'ok', subject: a.subject, userName: a.userName, details });
  }
}
