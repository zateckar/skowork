// Audit log (control-plane/identity Authentication Audit; admin actions for control-plane/admin-console).
import type { Db } from '../db.ts';
import { nowIso } from '../db.ts';

export interface AuditEntry {
  category: string;
  action: string;
  outcome: 'ok' | 'refused' | 'error';
  subject?: string | null;
  userName?: string | null;
  installationId?: string | null;
  sourceIp?: string | null;
  reason?: string | null;
  details?: unknown;
}

export interface AuditFilter {
  category?: string;
  action?: string;
  user?: string;
  outcome?: string;
  since?: string;
  until?: string;
}

export interface AuditRow {
  id: number;
  at: string;
  category: string;
  action: string;
  outcome: string;
  subject: string | null;
  user_name: string | null;
  installation_id: string | null;
  source_ip: string | null;
  reason: string | null;
  details: any;
}

/** The store is append-only: triggers refuse UPDATE and DELETE (control-plane/admin-console Audit Log Viewer). */
export class Audit {
  readonly #db: Db;
  constructor(db: Db) {
    this.#db = db;
  }

  write(e: AuditEntry): void {
    this.#db
      .prepare(
        `INSERT INTO audit (at, category, action, outcome, subject, user_name, installation_id, source_ip, reason, details)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
      )
      .run(
        nowIso(),
        e.category,
        e.action,
        e.outcome,
        e.subject ?? null,
        e.userName ?? null,
        e.installationId ?? null,
        e.sourceIp ?? null,
        e.reason ?? null,
        e.details === undefined ? null : JSON.stringify(e.details),
      );
  }

  query(f: AuditFilter & { limit?: number; offset?: number }, maxLimit = 500) {
    const where: string[] = [];
    const args: (string | number)[] = [];
    if (f.category) (where.push('category = ?'), args.push(f.category));
    if (f.action) (where.push('action = ?'), args.push(f.action));
    if (f.user) (where.push('(user_name = ? OR subject = ?)'), args.push(f.user, f.user));
    if (f.outcome) (where.push('outcome = ?'), args.push(f.outcome));
    if (f.since) (where.push('at >= ?'), args.push(f.since));
    if (f.until) (where.push('at < ?'), args.push(f.until));
    const limit = Math.min(Math.max(f.limit ?? 50, 1), maxLimit);
    const sql = `SELECT * FROM audit ${where.length ? 'WHERE ' + where.join(' AND ') : ''} ORDER BY id DESC LIMIT ? OFFSET ?`;
    const rows = this.#db.prepare(sql).all(...args, limit, f.offset ?? 0) as Record<string, unknown>[];
    return rows.map((r) => ({ ...r, details: r.details ? JSON.parse(String(r.details)) : null })) as AuditRow[];
  }
}
