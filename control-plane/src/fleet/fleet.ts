// control-plane/fleet-registry: installations, liveness, minimum versions, revocation, and the client and admin APIs.
import { Router, type Request } from 'express';
import type { Config } from '../config.ts';
import type { Db } from '../db.ts';
import { nowIso } from '../db.ts';
import { ApiError, clientIp, sendError, str } from '../http.ts';
import type { Audit } from '../identity/audit.ts';
import type { User } from '../identity/identity.ts';
import { requireRole } from '../identity/identity.ts';
import { addDenied } from '../identity/routes.ts';
import { PUSH_TYPES, type PushHub, type PushTarget, type PushType } from './push.ts';

const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const CHANNEL = /^[a-z0-9][a-z0-9_-]{0,31}$/;
const STATUSES = ['idle', 'running', 'paused'];

/** Parses MAJOR.MINOR.PATCH with an optional pre-release; returns null when invalid. */
export function parseVersion(v: unknown): [number, number, number, string] | null {
  if (typeof v !== 'string') return null;
  const m = /^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?$/.exec(v.trim());
  return m ? [Number(m[1]), Number(m[2]), Number(m[3]), m[4] ?? ''] : null;
}

export function compareVersions(a: string, b: string): number {
  const x = parseVersion(a);
  const y = parseVersion(b);
  if (!x || !y) return 0;
  for (let i = 0; i < 3; i++) if (x[i] !== y[i]) return (x[i] as number) - (y[i] as number);
  // A pre-release sorts before its release.
  if (x[3] === y[3]) return 0;
  if (!x[3]) return 1;
  if (!y[3]) return -1;
  return x[3] < y[3] ? -1 : 1;
}

export class FleetRegistry {
  readonly #cfg: Config;
  readonly #db: Db;
  readonly #audit: Audit;
  readonly #push: PushHub;

  constructor(cfg: Config, db: Db, audit: Audit, push: PushHub) {
    this.#cfg = cfg;
    this.#db = db;
    this.#audit = audit;
    this.#push = push;
  }

  minVersion(channel: string): string {
    const row = this.#db.prepare('SELECT version FROM min_versions WHERE channel = ?').get(channel) as { version: string } | undefined;
    return row?.version ?? '0.0.0';
  }

  register(user: User, body: Record<string, unknown>, ip: string) {
    const id = str(body.installation_id)?.toLowerCase() ?? '';
    if (!UUID.test(id) || /^0{8}-/.test(id)) throw new ApiError(400, 'INVALID_REQUEST', 'installation_id must be a UUID.');
    if (!parseVersion(body.version)) throw new ApiError(400, 'INVALID_REQUEST', 'version is missing or not a valid version number.');
    const channel = (str(body.channel)?.trim().toLowerCase() || this.#cfg.fleet.defaultChannel);
    if (!CHANNEL.test(channel)) throw new ApiError(400, 'INVALID_REQUEST', 'channel is not valid.');
    const existing = this.#db.prepare('SELECT subject, revoked_at FROM installations WHERE id = ?').get(id) as
      | { subject: string; revoked_at: string | null }
      | undefined;
    if (existing && existing.subject !== user.subject) {
      this.#audit.write({
        category: 'security', action: 'fleet.installation-conflict', outcome: 'refused', subject: user.subject, userName: user.userName,
        installationId: id, sourceIp: ip, reason: 'installation id is registered to a different user',
      });
      throw new ApiError(409, 'INSTALLATION_CONFLICT', 'This installation id belongs to a different user.');
    }
    const now = nowIso();
    const s = (k: string, max = 256) => (str(body[k]) ?? '').slice(0, max);
    // The user principal comes from the token only; a user_principal in the body is ignored.
    this.#db
      .prepare(
        `INSERT INTO installations (id, subject, user_name, groups, idp_roles, machine_name, version, channel, commit_id, os_build, hardware_class,
           source_ip, first_seen, last_seen)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
         ON CONFLICT (id) DO UPDATE SET user_name = excluded.user_name, groups = excluded.groups, idp_roles = excluded.idp_roles, machine_name = excluded.machine_name,
           version = excluded.version, channel = excluded.channel, commit_id = excluded.commit_id, os_build = excluded.os_build,
           hardware_class = excluded.hardware_class, source_ip = excluded.source_ip, last_seen = excluded.last_seen`,
      )
      .run(id, user.subject, user.userName, JSON.stringify(user.groups), JSON.stringify(user.idpRoles), s('machine_name'), s('version', 64), channel, s('commit', 64),
        s('os_build', 64), s('hardware_class'), ip, now, now);
    if (!existing) {
      this.#audit.write({ category: 'fleet', action: 'fleet.registered', outcome: 'ok', subject: user.subject, userName: user.userName, installationId: id, sourceIp: ip });
    }
    return { id, channel, revoked: Boolean(existing?.revoked_at), minVersion: this.minVersion(channel) };
  }

  heartbeat(user: User, body: Record<string, unknown>) {
    const id = str(body.installation_id)?.toLowerCase() ?? '';
    if (!UUID.test(id)) throw new ApiError(400, 'INVALID_REQUEST', 'installation_id must be a UUID.');
    const status = str(body.status)?.trim() || 'idle';
    if (!STATUSES.includes(status)) throw new ApiError(400, 'INVALID_REQUEST', 'status must be idle, running or paused.');
    const inst = this.#db.prepare('SELECT subject, channel FROM installations WHERE id = ?').get(id) as { subject: string; channel: string } | undefined;
    if (!inst) throw new ApiError(404, 'INSTALLATION_UNKNOWN', 'The installation is not registered; register first.');
    if (inst.subject !== user.subject) throw new ApiError(403, 'FORBIDDEN', 'The installation belongs to a different user.');
    // The network enforcement level (proxy-only or os-enforced) feeds policy simulation (control-plane/policy-service).
    const enforcement = str(body.network_enforcement);
    this.#db
      .prepare('UPDATE installations SET last_seen = ?, status = ?, policy_version = ?, groups = ?, idp_roles = ?, user_name = ?, network_enforcement = ? WHERE id = ?')
      .run(nowIso(), status, (str(body.policy_version) ?? '').slice(0, 64), JSON.stringify(user.groups), JSON.stringify(user.idpRoles), user.userName,
        enforcement === 'proxy-only' || enforcement === 'os-enforced' ? enforcement : '', id);
    return { minVersion: this.minVersion(inst.channel) };
  }

  setMinVersion(channel: string, version: string, by: string): number {
    channel = channel.trim().toLowerCase();
    if (!CHANNEL.test(channel)) throw new ApiError(400, 'INVALID_REQUEST', 'channel is not valid.');
    if (!parseVersion(version)) throw new ApiError(400, 'INVALID_REQUEST', 'version is not a valid version number.');
    this.#db
      .prepare(
        `INSERT INTO min_versions (channel, version, set_by, set_at) VALUES (?, ?, ?, ?)
         ON CONFLICT (channel) DO UPDATE SET version = excluded.version, set_by = excluded.set_by, set_at = excluded.set_at`,
      )
      .run(channel, version, by, nowIso());
    const installs = this.#db.prepare('SELECT id, version FROM installations WHERE channel = ? AND revoked_at IS NULL').all(channel) as {
      id: string;
      version: string;
    }[];
    let pushed = 0;
    for (const i of installs) {
      if (parseVersion(i.version) && compareVersions(i.version, version) < 0) {
        pushed += this.#push.send('minimum-version', { installation_id: i.id }, { channel, min_version: version }).recipients;
      }
    }
    return pushed;
  }

  /** Called when a deny-list entry is added: marks installations revoked, pushes revocation and closes connections. */
  revoke(kind: 'user' | 'installation', value: string): void {
    const now = nowIso();
    if (kind === 'installation') {
      this.#db.prepare('UPDATE installations SET revoked_at = COALESCE(revoked_at, ?) WHERE id = ?').run(now, value.toLowerCase());
      this.#push.send('revocation', { installation_id: value.toLowerCase() }, { scope: 'installation' });
      this.#push.closeRevoked({ installation_id: value });
    } else {
      this.#db.prepare('UPDATE installations SET revoked_at = COALESCE(revoked_at, ?) WHERE user_name = ? COLLATE NOCASE OR subject = ?').run(now, value, value);
      this.#push.send('revocation', { user: value }, { scope: 'user' });
      this.#push.closeRevoked({ user: value });
    }
  }

  list(q: Record<string, unknown>) {
    const where: string[] = [];
    const args: (string | number)[] = [];
    const add = (sql: string, v: unknown) => {
      if (typeof v === 'string' && v !== '') (where.push(sql), args.push(v));
    };
    add('version = ?', q.version);
    add('channel = ?', q.channel);
    add('policy_version = ?', q.policy_version);
    add('user_name = ? COLLATE NOCASE', q.user);
    add("machine_name LIKE '%' || ? || '%'", q.machine);
    add('EXISTS (SELECT 1 FROM json_each(installations.groups) g WHERE g.value = ?)', q.group);
    if (q.revoked === 'true') where.push('revoked_at IS NOT NULL');
    if (q.revoked === 'false') where.push('revoked_at IS NULL');
    const offlineBefore = new Date(Date.now() - this.#cfg.fleet.heartbeatIntervalS * this.#cfg.fleet.offlineAfterMissed * 1000).toISOString();
    const page = Math.max(1, Number(q.page ?? 1) || 1);
    const size = Math.min(500, Math.max(1, Number(q.page_size ?? 50) || 50));
    const rows = this.#db
      .prepare(`SELECT * FROM installations ${where.length ? 'WHERE ' + where.join(' AND ') : ''} ORDER BY last_seen DESC LIMIT ? OFFSET ?`)
      .all(...args, size, (page - 1) * size) as Record<string, unknown>[];
    const items = rows
      .map((r) => {
        const connected = this.#push.isConnected(String(r.id));
        const online = connected || String(r.last_seen) >= offlineBefore;
        return { ...r, groups: JSON.parse(String(r.groups)), connected, online };
      })
      .filter((r) => (q.online === 'true' ? r.online : q.online === 'false' ? !r.online : true));
    return { page, page_size: size, items };
  }

  /** Deletes installations not seen within the registry retention (Registry Data Retention). */
  prune(): void {
    const cutoff = new Date(Date.now() - this.#cfg.fleet.registryRetentionS * 1000).toISOString();
    this.#db.prepare('DELETE FROM installations WHERE last_seen < ?').run(cutoff);
  }
}

export function serviceUrls(base: string) {
  const ws = base.replace(/^http/i, 'ws');
  return {
    gateway: `${base}/llm/v1`,
    policy: `${base}/api/v1/policy`,
    skills: `${base}/api/v1/skills`,
    telemetry: `${base}/api/v1/telemetry`,
    tasks: `${base}/api/v1/tasks`,
    mcp: `${base}/api/v1/mcp`,
    push: `${ws}/api/v1/fleet/push`,
  };
}

function publicBase(cfg: Config, req: Request): string {
  return cfg.publicBaseUrl || `${req.protocol}://${req.get('host')}`;
}

export function fleetRoutes(cfg: Config, registry: FleetRegistry, deniedFor: (user: User, installationId: string) => boolean): Router {
  const r = Router();
  r.post('/api/v1/fleet/register', (req, res) => {
    const body = (req.body ?? {}) as Record<string, unknown>;
    if (deniedFor(req.user!, str(body.installation_id) ?? '')) {
      sendError(res, 403, 'DENIED', 'Access has been revoked.');
      return;
    }
    const reg = registry.register(req.user!, body, clientIp(req));
    res.json({
      installation_id: reg.id,
      heartbeat_interval_s: cfg.fleet.heartbeatIntervalS,
      min_version: reg.minVersion,
      revoked: reg.revoked,
      services: serviceUrls(publicBase(cfg, req)),
      server_time: nowIso(),
    });
  });
  r.post('/api/v1/fleet/heartbeat', (req, res) => {
    const body = (req.body ?? {}) as Record<string, unknown>;
    if (deniedFor(req.user!, str(body.installation_id) ?? '')) {
      sendError(res, 403, 'DENIED', 'Access has been revoked.');
      return;
    }
    const hb = registry.heartbeat(req.user!, body);
    res.json({ heartbeat_interval_s: cfg.fleet.heartbeatIntervalS, min_version: hb.minVersion });
  });
  return r;
}

export function fleetAdminRoutes(db: Db, audit: Audit, registry: FleetRegistry, push: PushHub): Router {
  const r = Router();
  const record = (req: Request, action: string, details: unknown) =>
    audit.write({ category: 'admin', action, outcome: 'ok', subject: req.user!.subject, userName: req.user!.userName, details });

  r.get('/installations', requireRole('security-auditor'), (req, res) => {
    res.json(registry.list(req.query as Record<string, unknown>));
  });
  r.post('/installations/:id/revoke', requireRole('platform-admin'), (req, res) => {
    const id = String(req.params.id).toLowerCase();
    if (!UUID.test(id)) {
      sendError(res, 400, 'INVALID_REQUEST', 'id must be a UUID.');
      return;
    }
    const reason = str(req.body?.reason)?.trim() || 'revoked by administrator';
    addDenied(db, 'installation', id, reason, req.user!.userName);
    registry.revoke('installation', id);
    record(req, 'fleet.installation.revoke', { installation_id: id, reason });
    res.json({ installation_id: id, revoked: true });
  });
  r.post('/users/revoke', requireRole('platform-admin'), (req, res) => {
    const user = str(req.body?.user)?.trim();
    if (!user) {
      sendError(res, 400, 'INVALID_REQUEST', 'user is required.');
      return;
    }
    const reason = str(req.body?.reason)?.trim() || 'revoked by administrator';
    addDenied(db, 'user', user, reason, req.user!.userName);
    registry.revoke('user', user);
    record(req, 'fleet.user.revoke', { user, reason });
    res.json({ user, revoked: true });
  });
  r.get('/min-versions', requireRole('security-auditor'), (_req, res) => {
    res.json(db.prepare('SELECT channel, version, set_by, set_at FROM min_versions ORDER BY channel').all());
  });
  r.put('/min-versions/:channel', requireRole('platform-admin'), (req, res) => {
    const version = str(req.body?.version);
    if (!version) {
      sendError(res, 400, 'INVALID_REQUEST', 'version is required.');
      return;
    }
    const pushed = registry.setMinVersion(String(req.params.channel), version, req.user!.userName);
    record(req, 'fleet.min-version.set', { channel: req.params.channel, version, pushed });
    res.json({ channel: String(req.params.channel).toLowerCase(), min_version: version, pushed });
  });
  // Diagnostics and tests: send a registered push type to an installation, a user or a Group.
  r.post('/push', requireRole('platform-admin'), (req, res) => {
    const type = str(req.body?.type) as PushType | undefined;
    if (!type || !PUSH_TYPES.includes(type)) {
      sendError(res, 400, 'INVALID_REQUEST', `type must be one of: ${[...PUSH_TYPES].sort().join(', ')}.`);
      return;
    }
    const t = (req.body?.target ?? {}) as Record<string, unknown>;
    const named = ['installation_id', 'user', 'group'].filter((k) => typeof t[k] === 'string' && t[k] !== '');
    if (named.length !== 1) {
      sendError(res, 400, 'INVALID_REQUEST', 'target must name exactly one of installation_id, user or group.');
      return;
    }
    const target = { [named[0]!]: t[named[0]!] } as PushTarget;
    const ttl = Number(req.body?.ttl_seconds) > 0 ? Number(req.body.ttl_seconds) : undefined;
    const result = push.send(type, target, req.body?.payload ?? {}, ttl);
    record(req, 'fleet.push.send', { type, target, recipients: result.recipients });
    res.json(result);
  });
  return r;
}
