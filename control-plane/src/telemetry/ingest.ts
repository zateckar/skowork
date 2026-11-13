// control-plane/telemetry-ingest (add-telemetry-and-admin design D1, D5): authenticated OTLP log ingest with an
// installation-to-user check, the catalogue allowlist re-applied per the sender's telemetry level, storage with
// retention, keyed pseudonyms, forwarding to the organization observability stack with a buffer, and per-installation
// rate limiting.
import { createCipheriv, createDecipheriv, createHmac, hkdfSync, randomBytes } from 'node:crypto';
import type { Request, Response } from 'express';
import type { Config } from '../config.ts';
import type { Db } from '../db.ts';
import { nowIso, tx } from '../db.ts';
import { clientIp, sendError } from '../http.ts';
import type { Audit } from '../identity/audit.ts';
import type { User } from '../identity/identity.ts';
import { ATTRIBUTES, EVENTS, RESOURCE, type AttrName, type EventName, type ResourceName } from './catalogue.gen.ts';
import { decodeLogs, encodeLogs, OtlpError, type LogBatch, type Value } from './otlp.ts';

export type TelemetryLevel = 'off' | 'metadata' | 'content';

export interface IngestStats {
  crashes: number;
  batches: number;
  records: number;
  rejected_spoofed: number;
  rate_limited: number;
  dropped_unknown: number;
  content_violations: number;
  forwarded_batches: number;
  forward_failures: number;
  forward_expired: number;
}

const DOMAIN = /^[a-z0-9.:-]{1,253}$/;

/** The host of a URL or host[:port][/path], lower case; '' when there is none (defence in depth for domain fields). */
export function domainOf(v: string): string {
  let s = v.trim();
  try {
    if (s.includes('://')) s = new URL(s).hostname;
  } catch {
    return '';
  }
  s = s.split(/[/?#]/)[0]!.replace(/^.*@/, '');
  if (s.startsWith('[')) s = s.slice(1, s.indexOf(']') > 0 ? s.indexOf(']') : s.length);
  else s = s.replace(/:\d*$/, '');
  s = s.toLowerCase();
  return DOMAIN.test(s) ? s : '';
}

const isAttr = (k: string): k is AttrName => Object.hasOwn(ATTRIBUTES, k);
const isEvent = (k: string): k is EventName => Object.hasOwn(EVENTS, k);
const isResource = (k: string): k is ResourceName => Object.hasOwn(RESOURCE, k);

function typed(type: string, v: Value): Value | undefined {
  switch (type) {
    case 'string':
      return typeof v === 'string' ? v : undefined;
    case 'domain':
      return typeof v === 'string' ? domainOf(v) || undefined : undefined;
    case 'integer':
      return typeof v === 'bigint' ? v : typeof v === 'number' && Number.isInteger(v) ? BigInt(v) : undefined;
    case 'number':
      return typeof v === 'number' ? v : typeof v === 'bigint' ? Number(v) : undefined;
    case 'boolean':
      return typeof v === 'boolean' ? v : undefined;
  }
  return undefined;
}

const jsonValue = (v: Value): string | number | boolean => (typeof v === 'bigint' ? Number(v) : v);

/** Token bucket per installation. */
class RateLimiter {
  readonly #buckets = new Map<string, { tokens: number; at: number }>();
  readonly perMinute: number;
  readonly burst: number;
  constructor(perMinute: number, burst: number) {
    this.perMinute = perMinute;
    this.burst = burst;
  }
  /** 0 when allowed, otherwise the seconds to wait. */
  take(key: string, now = Date.now()): number {
    const b = this.#buckets.get(key) ?? { tokens: this.burst, at: now };
    b.tokens = Math.min(this.burst, b.tokens + ((now - b.at) / 60_000) * this.perMinute);
    b.at = now;
    this.#buckets.set(key, b);
    if (b.tokens >= 1) {
      b.tokens -= 1;
      return 0;
    }
    return Math.max(1, Math.ceil(((1 - b.tokens) / this.perMinute) * 60));
  }
  prune(now = Date.now()): void {
    for (const [k, b] of this.#buckets) if (now - b.at > 600_000) this.#buckets.delete(k);
  }
}

export class TelemetryIngest {
  readonly #cfg: Config['telemetry'];
  readonly #db: Db;
  readonly #audit: Audit;
  readonly #levelOf: (u: User) => TelemetryLevel;
  readonly #limiter: RateLimiter;
  readonly #pseudonymKey: Buffer;
  readonly #crashKey: Buffer;
  readonly stats: IngestStats = {
    batches: 0, records: 0, rejected_spoofed: 0, rate_limited: 0, dropped_unknown: 0, content_violations: 0,
    forwarded_batches: 0, forward_failures: 0, forward_expired: 0, crashes: 0,
  };
  #forwarding = false;
  #fetch: typeof fetch = (...a) => fetch(...a);

  constructor(cfg: Config['telemetry'], db: Db, audit: Audit, levelOf: (u: User) => TelemetryLevel) {
    this.#cfg = cfg;
    this.#db = db;
    this.#audit = audit;
    this.#levelOf = levelOf;
    this.#limiter = new RateLimiter(cfg.batchesPerMinute, cfg.batchBurst);
    this.#pseudonymKey = Buffer.from(hkdfSync('sha256', cfg.secret, 'skowork-telemetry', 'pseudonym', 32));
    this.#crashKey = Buffer.from(hkdfSync('sha256', cfg.secret, 'skowork-telemetry', 'crash-dumps', 32));
  }

  /** Test seam for the forwarding target. */
  setFetch(f: typeof fetch): void {
    this.#fetch = f;
  }

  /** A stable keyed pseudonym (design D5); the mapping is kept for audited re-identification. */
  pseudonym(kind: 'user' | 'machine', value: string): string {
    const p = `${kind === 'user' ? 'u' : 'm'}_${createHmac('sha256', this.#pseudonymKey).update(`${kind}\0${value}`).digest('hex').slice(0, 20)}`;
    this.#db.prepare('INSERT OR IGNORE INTO telemetry_pseudonyms (pseudonym, kind, value, created_at) VALUES (?, ?, ?, ?)').run(p, kind, value, nowIso());
    return p;
  }

  reidentify(pseudonym: string): { kind: string; value: string } | null {
    const row = this.#db.prepare('SELECT kind, value FROM telemetry_pseudonyms WHERE pseudonym = ?').get(pseudonym) as { kind: string; value: string } | undefined;
    return row ? { kind: row.kind, value: row.value } : null;
  }

  /** The registered installation of the token's user, or an error response already sent (Authenticated OTLP Ingest). */
  #checkInstallation(req: Request, res: Response, installation: string): boolean {
    const user = req.user!;
    const inst = this.#db.prepare('SELECT user_name FROM installations WHERE id = ?').get(installation) as { user_name: string } | undefined;
    if (inst && inst.user_name === user.userName) return true;
    this.stats.rejected_spoofed++;
    this.#audit.write({
      category: 'security', action: 'telemetry.installation-mismatch', outcome: 'refused', subject: user.subject, userName: user.userName,
      installationId: installation, sourceIp: clientIp(req), reason: inst ? 'installation registered to another user' : 'installation not registered',
    });
    sendError(res, 403, 'INSTALLATION_MISMATCH', 'The installation is not registered to this user.');
    return false;
  }

  /** POST /api/v1/telemetry/v1/crashes: one minidump, metadata in x-skowork-crash-* headers (design D4). */
  handleCrash(req: Request, res: Response): void {
    const user = req.user!;
    const h = (k: string) => String(req.headers[`x-skowork-crash-${k}`] ?? '').slice(0, 128);
    const body = req.body;
    if (!(body instanceof Uint8Array) || body.length === 0) return sendError(res, 400, 'INVALID_REQUEST', 'The request body is empty.');
    if (body.length > this.#cfg.maxCrashBytes) return sendError(res, 413, 'PAYLOAD_TOO_LARGE', 'The crash dump is too large.');
    const id = h('id');
    const installation = h('installation_id');
    if (!/^[A-Za-z0-9_-]{8,64}$/.test(id) || !installation) return sendError(res, 400, 'INVALID_REQUEST', 'The crash report names no id or installation.');
    const wait = this.#limiter.take(installation);
    if (wait > 0) {
      this.stats.rate_limited++;
      return sendError(res, 429, 'RATE_LIMITED', 'Too many telemetry batches from this installation.', { retry_after_s: wait });
    }
    if (!this.#checkInstallation(req, res, installation)) return;
    if (this.#levelOf(user) === 'off') return sendError(res, 403, 'TELEMETRY_OFF', 'Telemetry is off for this user.');
    // Stored encrypted (AES-256-GCM); the tag follows the ciphertext.
    const iv = randomBytes(12);
    const cipher = createCipheriv('aes-256-gcm', this.#crashKey, iv);
    const data = Buffer.concat([cipher.update(body), cipher.final(), cipher.getAuthTag()]);
    const process = h('process').replace(/[^A-Za-z0-9_-]/g, '');
    this.#db.prepare(
      'INSERT OR IGNORE INTO telemetry_crashes (id, received_at, user_name, installation_id, process, size, iv, data) VALUES (?, ?, ?, ?, ?, ?, ?, ?)',
    ).run(id, nowIso(), user.userName, installation, process, body.length, iv, data);
    this.stats.crashes++;
    res.status(202).json({ id });
  }

  /** The decrypted dump of a stored crash report (for the security-auditor's audited access). */
  readCrash(id: string): Buffer | null {
    const row = this.#db.prepare('SELECT iv, data FROM telemetry_crashes WHERE id = ?').get(id) as { iv: Uint8Array; data: Uint8Array } | undefined;
    if (!row) return null;
    const data = Buffer.from(row.data);
    const decipher = createDecipheriv('aes-256-gcm', this.#crashKey, Buffer.from(row.iv));
    decipher.setAuthTag(data.subarray(data.length - 16));
    return Buffer.concat([decipher.update(data.subarray(0, data.length - 16)), decipher.final()]);
  }

  /** POST /api/v1/telemetry/v1/logs */  handleLogs(req: Request, res: Response): void {
    const user = req.user!;
    if (!String(req.headers['content-type'] ?? '').startsWith('application/x-protobuf'))
      return sendError(res, 415, 'INVALID_REQUEST', 'Telemetry is accepted as OTLP protobuf (application/x-protobuf).');
    const body = req.body;
    if (!(body instanceof Uint8Array) || body.length === 0) return sendError(res, 400, 'INVALID_REQUEST', 'The request body is empty.');

    let batch: LogBatch;
    try {
      batch = decodeLogs(body, { maxRecords: this.#cfg.maxRecordsPerBatch, maxAttributes: 64 });
    } catch (e) {
      return sendError(res, 400, 'INVALID_REQUEST', `The OTLP payload is malformed: ${e instanceof OtlpError ? e.message : 'decode error'}.`);
    }
    const installation = batch.resource.get('skowork.installation_id');
    if (typeof installation !== 'string' || !installation) return sendError(res, 400, 'INVALID_REQUEST', 'The batch names no installation (skowork.installation_id).');

    const wait = this.#limiter.take(installation);
    if (wait > 0) {
      this.stats.rate_limited++;
      return sendError(res, 429, 'RATE_LIMITED', 'Too many telemetry batches from this installation.', { retry_after_s: wait });
    }

    // The installation must be registered to the token's user (Authenticated OTLP Ingest).
    const inst = this.#db.prepare('SELECT user_name, machine_name, groups FROM installations WHERE id = ?').get(installation) as
      | { user_name: string; machine_name: string; groups: string }
      | undefined;
    if (!inst || inst.user_name !== user.userName) {
      this.stats.rejected_spoofed++;
      this.#audit.write({
        category: 'security', action: 'telemetry.installation-mismatch', outcome: 'refused', subject: user.subject, userName: user.userName,
        installationId: installation, sourceIp: clientIp(req), reason: inst ? 'installation registered to another user' : 'installation not registered',
      });
      return sendError(res, 403, 'INSTALLATION_MISMATCH', 'The installation is not registered to this user.');
    }

    const level = this.#levelOf(user);
    const accepted = this.#filter(batch, level);
    const receivedAt = nowIso();
    const groups = JSON.stringify(user.groups);
    tx(this.#db, () => {
      const ins = this.#db.prepare(
        'INSERT INTO telemetry_records (received_at, at, user_name, installation_id, groups, event, attrs) VALUES (?, ?, ?, ?, ?, ?, ?)',
      );
      for (const r of accepted.records) {
        const at = new Date(Number(r.timeUnixNano / 1_000_000n) || Date.now()).toISOString();
        const attrs = Object.fromEntries([...r.attributes].map(([k, v]) => [k, jsonValue(v)]));
        ins.run(receivedAt, at, user.userName, installation, groups, r.eventName, JSON.stringify(attrs));
      }
      if (this.#cfg.forwardUrl && accepted.records.length > 0) {
        const fwd: LogBatch = { ...accepted, resource: new Map(accepted.resource) };
        fwd.resource.set('enduser.id', this.#cfg.pseudonymize ? this.pseudonym('user', user.userName) : user.userName);
        if (inst.machine_name) fwd.resource.set('host.name', this.#cfg.pseudonymize ? this.pseudonym('machine', inst.machine_name) : inst.machine_name);
        this.#db.prepare('INSERT INTO telemetry_outbox (created_at, payload) VALUES (?, ?)').run(receivedAt, encodeLogs(fwd));
      }
    });
    this.stats.batches++;
    this.stats.records += accepted.records.length;
    res.json({ accepted: accepted.records.length, level });
  }

  /** The batch reduced to catalogue fields allowed at `level` (Level Enforcement At Ingest). */
  #filter(batch: LogBatch, level: TelemetryLevel): LogBatch {
    const out: LogBatch = { resource: new Map(), scopeName: batch.scopeName.slice(0, 64), scopeVersion: batch.scopeVersion.slice(0, 64), records: [] };
    for (const [k, v] of batch.resource) {
      if (!isResource(k)) {
        this.stats.dropped_unknown++;
        continue;
      }
      const t = typed(RESOURCE[k].type, v);
      if (t !== undefined) out.resource.set(k, typeof t === 'string' ? t.slice(0, 256) : t);
    }
    if (level === 'off') return out;
    for (const r of batch.records) {
      if (!isEvent(r.eventName)) {
        this.stats.dropped_unknown++;
        continue;
      }
      const allowed: readonly string[] = EVENTS[r.eventName];
      const attrs = new Map<string, Value>();
      for (const [k, v] of r.attributes) {
        if (!isAttr(k) || !allowed.includes(k)) {
          this.stats.dropped_unknown++;
          continue;
        }
        const def = ATTRIBUTES[k];
        if (def.level === 'content' && level !== 'content') {
          this.stats.content_violations++;
          continue;
        }
        const t = typed(def.type, v);
        if (t === undefined) continue;
        attrs.set(k, typeof t === 'string' ? t.slice(0, def.level === 'content' ? 8000 : 256) : t);
      }
      out.records.push({ timeUnixNano: r.timeUnixNano, eventName: r.eventName, attributes: attrs });
    }
    return out;
  }

  /** Sends buffered batches to the forwarding target in order; stops at the first failure (Forwarding). */
  async forwardOnce(): Promise<void> {
    if (!this.#cfg.forwardUrl || this.#forwarding) return;
    this.#forwarding = true;
    try {
      for (;;) {
        const rows = this.#db.prepare('SELECT id, payload FROM telemetry_outbox ORDER BY id LIMIT 20').all() as { id: number; payload: Uint8Array }[];
        if (rows.length === 0) return;
        for (const row of rows) {
          let ok = false;
          try {
            const r = await this.#fetch(this.#cfg.forwardUrl, {
              method: 'POST',
              headers: { 'content-type': 'application/x-protobuf', ...this.#cfg.forwardHeaders },
              body: new Uint8Array(row.payload),
              signal: AbortSignal.timeout(10_000),
            });
            ok = r.ok;
          } catch {
            ok = false;
          }
          if (!ok) {
            this.stats.forward_failures++;
            return;
          }
          this.#db.prepare('DELETE FROM telemetry_outbox WHERE id = ?').run(row.id);
          this.stats.forwarded_batches++;
        }
      }
    } finally {
      this.#forwarding = false;
    }
  }

  /** Retention (Telemetry Retention) and the forwarding buffer limit. */
  prune(now = Date.now()): void {
    const iso = (ms: number) => new Date(now - ms).toISOString();
    this.#db.prepare('DELETE FROM telemetry_records WHERE received_at < ?').run(iso(this.#cfg.retentionDays * 86_400_000));
    this.#db.prepare('DELETE FROM telemetry_crashes WHERE received_at < ?').run(iso(this.#cfg.crashRetentionDays * 86_400_000));
    const expired = this.#db.prepare('DELETE FROM telemetry_outbox WHERE created_at < ?').run(iso(this.#cfg.forwardBufferHours * 3_600_000));
    this.stats.forward_expired += Number(expired.changes);
    this.#limiter.prune(now);
  }

  outboxSize(): number {
    return Number((this.#db.prepare('SELECT COUNT(*) AS n FROM telemetry_outbox').get() as { n: number }).n);
  }
}
