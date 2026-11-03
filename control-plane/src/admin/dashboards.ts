// Aggregate usage dashboards (control-plane/admin-console Aggregate Usage Dashboards; design D7). Figures are per
// Group and never per user; every cell with fewer than K distinct users is suppressed.
import type { Db } from '../db.ts';

export const K_FLOOR = 5;

class Cell {
  readonly users = new Set<string>();
  n = 0;
  errors = 0;
  add(user: string, n: number, error = false) {
    this.users.add(user.toLowerCase());
    this.n += n;
    if (error) this.errors += n;
  }
}

const cellOf = <K>(m: Map<K, Cell>, k: K) => {
  let c = m.get(k);
  if (!c) m.set(k, (c = new Cell()));
  return c;
};

interface GroupData {
  active: Set<string>;
  installations: Cell;
  versions: Map<string, Cell>;
  runs: Cell;
  cancelled: number;
  tools: Map<string, Cell>;
  clientErrors: Cell;
  tokens: Cell;
}

export function dashboards(db: Db, opts: { days: number; budgetOf: (group: string) => { tokens: number | null; period: string | null } }) {
  const days = Math.min(Math.max(Math.trunc(opts.days) || 7, 1), 90);
  const until = new Date();
  const since = new Date(until.getTime() - days * 86_400_000).toISOString();
  const groups = new Map<string, GroupData>();
  const g = (name: string) => {
    let d = groups.get(name);
    if (!d) {
      d = { active: new Set(), installations: new Cell(), versions: new Map(), runs: new Cell(), cancelled: 0, tools: new Map(), clientErrors: new Cell(), tokens: new Cell() };
      groups.set(name, d);
    }
    return d;
  };

  const inst = db
    .prepare(
      `SELECT g.value AS grp, i.user_name AS u, i.version AS version, COUNT(*) AS n FROM installations i, json_each(i.groups) g
       WHERE i.revoked_at IS NULL AND i.last_seen >= ? GROUP BY 1, 2, 3`,
    )
    .all(since) as { grp: string; u: string; version: string; n: number }[];
  for (const r of inst) {
    const d = g(r.grp);
    d.active.add(r.u.toLowerCase());
    d.installations.add(r.u, r.n);
    cellOf(d.versions, r.version || 'unknown').add(r.u, r.n);
  }

  const events = db
    .prepare(
      `SELECT g.value AS grp, t.user_name AS u, t.event AS event,
              json_extract(t.attrs, '$."run.outcome"') AS run_outcome, json_extract(t.attrs, '$."tool.id"') AS tool,
              json_extract(t.attrs, '$."tool.outcome"') AS tool_outcome, COUNT(*) AS n
       FROM telemetry_records t, json_each(t.groups) g
       WHERE t.at >= ? AND t.event IN ('run.finished', 'tool.call', 'client.error') GROUP BY 1, 2, 3, 4, 5, 6`,
    )
    .all(since) as { grp: string; u: string; event: string; run_outcome: string | null; tool: string | null; tool_outcome: string | null; n: number }[];
  for (const r of events) {
    const d = g(r.grp);
    d.active.add(r.u.toLowerCase());
    if (r.event === 'run.finished') {
      d.runs.add(r.u, r.n, r.run_outcome === 'failed');
      if (r.run_outcome === 'cancelled') d.cancelled += r.n;
    } else if (r.event === 'tool.call') {
      cellOf(d.tools, r.tool ?? 'unknown').add(r.u, r.n, r.tool_outcome === 'error');
    } else {
      d.clientErrors.add(r.u, r.n);
    }
  }

  const usage = db
    .prepare(`SELECT g.value AS grp, l.user_name AS u, SUM(l.total_tokens) AS n FROM llm_usage l, json_each(l.groups) g WHERE l.at >= ? GROUP BY 1, 2`)
    .all(since) as { grp: string; u: string; n: number }[];
  for (const r of usage) {
    const d = g(r.grp);
    d.active.add(r.u.toLowerCase());
    d.tokens.add(r.u, r.n);
  }

  const ok = (c: Cell) => c.users.size >= K_FLOOR;
  // Breakdown cells below the floor are merged into one suppressed entry that names none of them.
  const floor = <T>(cells: [string, Cell][], show: (e: [string, Cell]) => T) => {
    const shown = cells.filter(([, c]) => ok(c)).map(show);
    return { cells: shown, suppressed: cells.length - shown.length };
  };
  const rows = [...groups.keys()].sort().map((name) => {
    const d = groups.get(name)!;
    // A Group below the floor shows nothing, not even its user count.
    if (d.active.size < K_FLOOR) return { group: name, suppressed: true as const };
    const budget = opts.budgetOf(name);
    return {
      group: name,
      suppressed: false as const,
      active_users: d.active.size,
      active_installations: ok(d.installations) ? d.installations.n : null,
      runs: ok(d.runs)
        ? { total: d.runs.n, failed: d.runs.errors, cancelled: d.cancelled, error_rate: d.runs.n ? d.runs.errors / d.runs.n : 0 }
        : null,
      tools: floor([...d.tools].sort((a, b) => a[0].localeCompare(b[0])), ([tool, c]) => ({ tool, calls: c.n, errors: c.errors })),
      client_errors: ok(d.clientErrors) ? d.clientErrors.n : null,
      tokens: ok(d.tokens)
        ? { used: d.tokens.n, users: d.tokens.users.size, budget_per_user: budget.tokens, budget_period: budget.period,
            quota_used: budget.tokens ? d.tokens.n / (budget.tokens * d.tokens.users.size) : null }
        : null,
      versions: floor([...d.versions].sort((a, b) => b[0].localeCompare(a[0], undefined, { numeric: true })), ([version, c]) => ({ version, installations: c.n })),
    };
  });
  return { since, until: until.toISOString(), days, k: K_FLOOR, groups: rows };
}
