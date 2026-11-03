// Admin console API of its own (control-plane/admin-console): who am I, the audit log viewer and export, individual-level
// data access with a mandatory reason, the access records for the works council, and the aggregate dashboards.
// The management views call the administration APIs of the other modules directly.
import { Router, type Request } from 'express';
import type { Db } from '../db.ts';
import { ApiError, str } from '../http.ts';
import type { Audit, AuditFilter } from '../identity/audit.ts';
import type { TelemetryIngest } from '../telemetry/ingest.ts';
import { requireView, viewsFor } from './authz.ts';
import { dashboards } from './dashboards.ts';

const EXPORT_MAX = 50_000;

export function adminConsoleRoutes(db: Db, audit: Audit, telemetry: TelemetryIngest, budgetOf: (group: string) => { tokens: number | null; period: string | null }): Router {
  const r = Router();

  r.get('/me', (req, res) => {
    const u = req.user!;
    res.json({ user_name: u.userName, display_name: u.displayName, groups: u.groups, admin_roles: u.adminRoles, views: viewsFor(u.adminRoles) });
  });

  // ---- individual-level access (Individual-Level Data Access) ------------------------------------------------------
  const reasonOf = (req: Request): string => {
    const reason = str(req.query.reason)?.trim();
    if (!reason || reason.length > 500) throw new ApiError(400, 'REASON_REQUIRED', 'State the reason for this access (1 to 500 characters).');
    return reason;
  };
  /** Recorded before any data is read; the works-council-reviewer reviews these records. */
  const recordAccess = (req: Request, kind: string, user: string | null, reason: string, details: Record<string, unknown> = {}) =>
    audit.write({
      category: 'individual-access', action: `individual.${kind}`, outcome: 'ok', subject: req.user!.subject, userName: req.user!.userName,
      reason, details: { user, ...details },
    });

  const filterOf = (req: Request): AuditFilter => ({
    category: str(req.query.category),
    action: str(req.query.action),
    user: str(req.query.user)?.trim() || undefined,
    outcome: str(req.query.outcome),
    since: str(req.query.since),
    until: str(req.query.until),
  });
  /** Audit events searched by user are individual-level data. */
  const auditFilter = (req: Request, kind: string): AuditFilter => {
    const f = filterOf(req);
    if (f.user) recordAccess(req, kind, f.user, reasonOf(req), { filter: f });
    return f;
  };

  // ---- audit log (Audit Log Viewer) ---------------------------------------------------------------------------------
  r.get('/audit', requireView('audit'), (req, res) => {
    const f = auditFilter(req, 'audit');
    res.json(audit.query({ ...f, limit: req.query.limit ? Number(req.query.limit) : undefined, offset: req.query.offset ? Number(req.query.offset) : undefined }));
  });
  r.get('/audit/export', requireView('audit'), (req, res) => {
    const format = str(req.query.format) ?? 'jsonl';
    if (format !== 'jsonl' && format !== 'csv') throw new ApiError(400, 'INVALID_REQUEST', 'format is jsonl or csv.');
    const f = auditFilter(req, 'audit-export');
    const rows = audit.query({ ...f, limit: EXPORT_MAX }, EXPORT_MAX);
    audit.write({
      category: 'admin', action: 'audit.export', outcome: 'ok', subject: req.user!.subject, userName: req.user!.userName,
      details: { filter: f, format, events: rows.length },
    });
    const stamp = new Date().toISOString().replace(/[:.]/g, '-');
    res.setHeader('Content-Disposition', `attachment; filename="workbench-audit-${stamp}.${format}"`);
    if (format === 'jsonl') {
      res.type('application/x-ndjson').send(rows.map((x) => JSON.stringify(x)).join('\n') + (rows.length ? '\n' : ''));
      return;
    }
    const cols = ['id', 'at', 'category', 'action', 'outcome', 'subject', 'user_name', 'installation_id', 'source_ip', 'reason', 'details'] as const;
    const cell = (v: unknown) => {
      const s = v === null || v === undefined ? '' : typeof v === 'object' ? JSON.stringify(v) : String(v);
      // Quoted, and a leading formula character is neutralised so a spreadsheet does not evaluate it.
      return `"${(/^[=+\-@]/.test(s) ? `'${s}` : s).replace(/"/g, '""')}"`;
    };
    res.type('text/csv').send([cols.join(','), ...rows.map((x) => cols.map((c) => cell(x[c])).join(','))].join('\r\n') + '\r\n');
  });

  // ---- individual-level telemetry, crash dumps and re-identification ------------------------------------------------
  r.get('/individual/telemetry', requireView('individual'), (req, res) => {
    const user = str(req.query.user)?.trim();
    if (!user) throw new ApiError(400, 'INVALID_REQUEST', 'user is required.');
    const reason = reasonOf(req);
    recordAccess(req, 'telemetry', user, reason);
    const records = (
      db.prepare('SELECT id, at, installation_id, event, attrs FROM telemetry_records WHERE user_name = ? COLLATE NOCASE ORDER BY at DESC, id DESC LIMIT 1000').all(user) as {
        attrs: string;
      }[]
    ).map((x) => ({ ...x, attrs: JSON.parse(x.attrs) }));
    const crashes = db
      .prepare('SELECT id, received_at, installation_id, process, size FROM telemetry_crashes WHERE user_name = ? COLLATE NOCASE ORDER BY received_at DESC')
      .all(user);
    res.json({ user, records, crashes });
  });
  r.get('/individual/crashes/:id', requireView('individual'), (req, res) => {
    const reason = reasonOf(req);
    const meta = db.prepare('SELECT user_name FROM telemetry_crashes WHERE id = ?').get(String(req.params.id)) as { user_name: string } | undefined;
    if (!meta) throw new ApiError(404, 'NOT_FOUND', 'No such crash report.');
    recordAccess(req, 'crash-dump', meta.user_name, reason, { crash_id: req.params.id });
    const dump = telemetry.readCrash(String(req.params.id));
    if (!dump) throw new ApiError(404, 'NOT_FOUND', 'No such crash report.');
    res.setHeader('Content-Disposition', `attachment; filename="${String(req.params.id).replace(/[^A-Za-z0-9_-]/g, '')}.dmp"`);
    res.type('application/octet-stream').send(dump);
  });
  r.get('/individual/pseudonyms/:pseudonym', requireView('individual'), (req, res) => {
    const reason = reasonOf(req);
    const pseudonym = String(req.params.pseudonym);
    const found = telemetry.reidentify(pseudonym);
    recordAccess(req, 're-identification', found?.kind === 'user' ? found.value : null, reason, { pseudonym, kind: found?.kind ?? null });
    if (!found) throw new ApiError(404, 'NOT_FOUND', 'No such pseudonym.');
    res.json({ pseudonym, ...found });
  });

  // ---- access records (works council review) ------------------------------------------------------------------------
  r.get('/access-records', requireView('access-records'), (req, res) => {
    const rows = audit.query({ category: 'individual-access', since: str(req.query.since), until: str(req.query.until), limit: Number(req.query.limit) || 200, offset: Number(req.query.offset) || 0 });
    res.json(
      rows.map((x) => {
        const d = (x.details ?? {}) as { user?: string | null };
        return { id: x.id, at: x.at, auditor: x.user_name, user: d.user ?? null, kind: String(x.action).replace(/^individual\./, ''), reason: x.reason };
      }),
    );
  });

  // ---- aggregate dashboards -----------------------------------------------------------------------------------------
  r.get('/dashboards', requireView('dashboards'), (req, res) => {
    res.json(dashboards(db, { days: Number(req.query.days) || 7, budgetOf }));
  });
  return r;
}
