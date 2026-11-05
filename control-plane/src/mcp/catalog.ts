// control-plane/mcp-catalog: approved MCP servers, their Group assignment, tool discovery and review, versions,
// packages, and what control-plane/policy-service delivers as policy.mcp.servers (add-core-tools-and-mcp design D5, D6).
import { createHash } from 'node:crypto';
import type { Config } from '../config.ts';
import type { Db } from '../db.ts';
import { nowIso, tx } from '../db.ts';
import { ApiError } from '../http.ts';
import type { Audit } from '../identity/audit.ts';
import { EVERYONE, type User } from '../identity/identity.ts';
import { type DiscoveredTool, DiscoveryError, MCP_PROTOCOL_VERSION, discoverTools } from './discovery.ts';

export const RISK_CLASSES = ['read', 'write-local', 'external', 'destructive'] as const;
export type RiskClass = (typeof RISK_CLASSES)[number];

export interface Entry {
  id: string;
  name: string;
  owner: string;
  kind: 'remote' | 'local';
  spec_version: string;
  url?: string;
  auth?: 'user-token' | 'none';
  package?: string;
  args: string[];
  env: Record<string, string>;
  include_tools: string[];
  exclude_tools: string[];
  risk: Record<string, RiskClass>;
}

interface ServerRow {
  id: string;
  entry: string;
  enabled: number;
  discovery_status: 'none' | 'ok' | 'failed';
  discovery_error: string | null;
  discovered_at: string | null;
}

interface ToolRow {
  name: string;
  description: string;
  annotations: string;
  status: 'baseline' | 'pending' | 'included' | 'excluded';
  present: number;
  first_seen: string;
  last_seen: string;
  decided_by: string | null;
  decided_at: string | null;
}

type Actor = Pick<User, 'subject' | 'userName'>;

const ID = /^[a-z0-9][a-z0-9-]{0,62}$/;
const GROUP = /^[a-z0-9][a-z0-9_-]{0,63}$/;
const SHA256 = /^[0-9a-f]{64}$/;

const invalid = (field: string, message: string) => new ApiError(400, 'VALIDATION_FAILED', message, { field });

const strings = (v: unknown, field: string): string[] => {
  if (v === undefined) return [];
  if (!Array.isArray(v) || v.some((s) => typeof s !== 'string' || s === '')) throw invalid(field, `${field} must be a list of non-empty strings.`);
  return [...new Set(v as string[])];
};

/** Tool-name pattern of an include or exclude list: exact, or with `*` wildcards. */
export function matchTool(pattern: string, name: string): boolean {
  if (!pattern.includes('*')) return pattern === name;
  const re = new RegExp(`^${pattern.split('*').map((p) => p.replace(/[.+?^${}()|[\]\\]/g, '\\$&')).join('.*')}$`);
  return re.test(name);
}

export class McpCatalog {
  readonly #cfg: Config;
  readonly #db: Db;
  readonly #audit: Audit;
  readonly #onChanged: (groups: string[] | 'everyone') => void;
  #cache = new Map<string, { at: number; entries: unknown[] }>();

  constructor(
    cfg: Config,
    db: Db,
    audit: Audit,
    hooks: { onChanged: (groups: string[] | 'everyone') => void },
  ) {
    this.#cfg = cfg;
    this.#db = db;
    this.#audit = audit;
    this.#onChanged = hooks.onChanged;
  }

  // ---- validation -----------------------------------------------------------------------------------------------------

  /** Catalog Entry Rules. `id` is taken from the path on update. */
  validate(body: Record<string, unknown>): Entry {
    const id = body.id;
    if (typeof id !== 'string' || !ID.test(id)) throw invalid('id', `The id "${String(id)}" must be lower-case letters, digits and hyphens.`);
    const name = typeof body.name === 'string' && body.name.trim() ? body.name.trim() : id;
    const owner = typeof body.owner === 'string' ? body.owner.trim() : '';
    if (!owner) throw invalid('owner', 'owner is required.');
    const kind = body.kind;
    if (kind !== 'remote' && kind !== 'local') throw invalid('kind', 'kind must be remote or local.');
    const spec_version = typeof body.spec_version === 'string' && body.spec_version ? body.spec_version : MCP_PROTOCOL_VERSION;
    const risk: Record<string, RiskClass> = {};
    if (body.risk !== undefined) {
      if (!body.risk || typeof body.risk !== 'object' || Array.isArray(body.risk)) throw invalid('risk', 'risk must map tool names to risk classes.');
      for (const [tool, r] of Object.entries(body.risk as Record<string, unknown>)) {
        if (!RISK_CLASSES.includes(r as RiskClass)) throw invalid('risk', `The risk class of ${tool} must be one of ${RISK_CLASSES.join(', ')}.`);
        risk[tool] = r as RiskClass;
      }
    }
    const e: Entry = {
      id, name, owner, kind, spec_version,
      args: [], env: {},
      include_tools: strings(body.include_tools, 'include_tools'),
      exclude_tools: strings(body.exclude_tools, 'exclude_tools'),
      risk,
    };
    if (kind === 'remote') {
      let url: URL;
      try {
        url = new URL(String(body.url ?? ''));
      } catch {
        throw invalid('url', 'A remote entry needs a valid URL.');
      }
      if (url.protocol !== 'https:' && !(url.protocol === 'http:' && this.#cfg.mcpCatalog.allowInsecureHttp)) {
        throw invalid('url', `The URL ${url.href} must use HTTPS.`);
      }
      const auth = body.auth ?? 'user-token';
      if (auth !== 'user-token' && auth !== 'none') throw invalid('auth', 'auth must be user-token or none.');
      e.url = url.href;
      e.auth = auth;
    } else {
      const sha = typeof body.package === 'string' ? body.package.toLowerCase() : '';
      if (!SHA256.test(sha) || !this.#db.prepare('SELECT 1 FROM mcp_packages WHERE sha256 = ?').get(sha)) {
        throw invalid('package', `No package with the hash ${String(body.package ?? '')} was uploaded.`);
      }
      e.package = sha;
      e.args = strings(body.args, 'args');
      if (body.env !== undefined) {
        if (!body.env || typeof body.env !== 'object' || Array.isArray(body.env) || Object.values(body.env).some((v) => typeof v !== 'string')) {
          throw invalid('env', 'env must map names to string values.');
        }
        e.env = { ...(body.env as Record<string, string>) };
      }
    }
    return e;
  }

  #groups(v: unknown): string[] | undefined {
    if (v === undefined) return undefined;
    const gs = strings(v, 'groups');
    for (const g of gs) if (!GROUP.test(g)) throw invalid('groups', `The Group name "${g}" is not valid.`);
    return gs.sort();
  }

  // ---- reads ----------------------------------------------------------------------------------------------------------

  #row(id: string): ServerRow | undefined {
    return this.#db.prepare('SELECT * FROM mcp_servers WHERE id = ?').get(id) as ServerRow | undefined;
  }

  #assigned(id: string): string[] {
    return (this.#db.prepare('SELECT workbench_group FROM mcp_assignments WHERE server_id = ? ORDER BY workbench_group').all(id) as { workbench_group: string }[]).map((r) => r.workbench_group);
  }

  #tools(id: string): ToolRow[] {
    return this.#db.prepare('SELECT * FROM mcp_tools WHERE server_id = ? ORDER BY name').all(id) as unknown as ToolRow[];
  }

  #view(row: ServerRow) {
    return {
      ...(JSON.parse(row.entry) as Entry),
      groups: this.#assigned(row.id),
      enabled: row.enabled === 1,
      discovery: { status: row.discovery_status, error: row.discovery_error, at: row.discovered_at },
      tools: this.#tools(row.id).map((t) => ({
        name: t.name, description: t.description, annotations: JSON.parse(t.annotations), status: t.status, present: t.present === 1,
        first_seen: t.first_seen, last_seen: t.last_seen, decided_by: t.decided_by, decided_at: t.decided_at,
      })),
    };
  }

  list() {
    return (this.#db.prepare('SELECT * FROM mcp_servers ORDER BY id').all() as unknown as ServerRow[]).map((r) => this.#view(r));
  }

  get(id: string) {
    const row = this.#row(id);
    if (!row) throw new ApiError(404, 'NOT_FOUND', `No catalog entry ${id}.`);
    return this.#view(row);
  }

  versions(id: string) {
    return (this.#db.prepare('SELECT id, action, snapshot, changed_by, changed_at FROM mcp_server_versions WHERE server_id = ? ORDER BY id DESC').all(id) as {
      id: number; action: string; snapshot: string | null; changed_by: string; changed_at: string;
    }[]).map((v) => ({ ...v, snapshot: v.snapshot ? JSON.parse(v.snapshot) : null }));
  }

  /** The catalog stamp: the id of the latest version (0 for an empty catalog). */
  stamp(): number {
    return (this.#db.prepare('SELECT COALESCE(MAX(id), 0) AS s FROM mcp_server_versions').get() as { s: number }).s;
  }

  // ---- delivery -------------------------------------------------------------------------------------------------------

  #enabledFor(groups: string[]): ServerRow[] {
    const gs = [...new Set([EVERYONE, ...groups])];
    return this.#db
      .prepare(
        `SELECT DISTINCT s.* FROM mcp_servers s JOIN mcp_assignments a ON a.server_id = s.id
         WHERE s.enabled = 1 AND a.workbench_group IN (${gs.map(() => '?').join(',')}) ORDER BY s.id`,
      )
      .all(...gs) as unknown as ServerRow[];
  }

  /** Entry Delivered In The Policy Bundle: what the client needs, with tools awaiting review excluded. */
  entriesFor(groups: string[]): unknown[] {
    const key = [...groups].sort().join(',');
    const hit = this.#cache.get(key);
    if (hit && Date.now() - hit.at < this.#cfg.mcpCatalog.cacheSeconds * 1000) return hit.entries;
    const entries = this.#enabledFor(groups).map((row) => {
      const e = JSON.parse(row.entry) as Entry;
      const tools = this.#tools(row.id);
      const exclude = new Set(e.exclude_tools);
      for (const t of tools) if (t.status === 'pending' || t.status === 'excluded') exclude.add(t.name);
      const include = [...e.include_tools];
      if (include.length) {
        for (const t of tools) if (t.status === 'included' && !include.some((p) => matchTool(p, t.name))) include.push(t.name);
      }
      const out: Record<string, unknown> = { id: e.id, name: e.name, kind: e.kind, spec_version: e.spec_version };
      if (e.kind === 'remote') Object.assign(out, { url: e.url, auth: e.auth });
      else Object.assign(out, { package: e.package, args: e.args, env: e.env });
      return { ...out, include_tools: include, exclude_tools: [...exclude].sort(), risk: e.risk };
    });
    this.#cache.set(key, { at: Date.now(), entries });
    return entries;
  }

  // ---- changes --------------------------------------------------------------------------------------------------------

  #changed(actor: Actor, id: string, action: string, before: unknown, groupsBefore: string[]): void {
    const row = this.#row(id);
    const after = row ? this.#view(row) : null;
    this.#db
      .prepare('INSERT INTO mcp_server_versions (server_id, action, snapshot, changed_by, changed_at) VALUES (?, ?, ?, ?, ?)')
      .run(id, action, after ? JSON.stringify(after) : null, actor.userName, nowIso());
    this.#audit.write({ category: 'admin', action, outcome: 'ok', subject: actor.subject, userName: actor.userName, details: { id, before, after } });
    this.#cache.clear();
    const affected = new Set([...groupsBefore, ...(after?.groups ?? [])]);
    if (affected.has(EVERYONE)) this.#onChanged('everyone');
    else if (affected.size) this.#onChanged([...affected]);
  }

  /** Runs discovery and stores the snapshot (Tool Discovery Snapshot, Tool Review Decisions, Discovery Failure). */
  async #discover(e: Entry, token: string | null): Promise<{ ok: true; tools: DiscoveredTool[] } | { ok: false; error: string }> {
    try {
      if (e.auth === 'user-token' && !token) throw new DiscoveryError('no access token of the administrator to present');
      return { ok: true, tools: await discoverTools(e.url!, { token: e.auth === 'user-token' ? token! : undefined, timeoutMs: this.#cfg.mcpCatalog.discoveryTimeoutMs, specVersion: e.spec_version }) };
    } catch (err) {
      return { ok: false, error: err instanceof DiscoveryError ? err.message : `discovery failed: ${(err as Error).message}` };
    }
  }

  #storeDiscovery(id: string, r: { ok: true; tools: DiscoveredTool[] } | { ok: false; error: string }): void {
    const now = nowIso();
    if (!r.ok) {
      this.#db.prepare("UPDATE mcp_servers SET discovery_status = 'failed', discovery_error = ?, discovered_at = ?, enabled = 0 WHERE id = ?").run(r.error, now, id);
      return;
    }
    this.#db.prepare("UPDATE mcp_servers SET discovery_status = 'ok', discovery_error = NULL, discovered_at = ? WHERE id = ?").run(now, id);
    const known = new Map(this.#tools(id).map((t) => [t.name, t]));
    const baseline = known.size === 0;
    const seen = new Set<string>();
    for (const t of r.tools) {
      seen.add(t.name);
      if (known.has(t.name)) {
        this.#db
          .prepare('UPDATE mcp_tools SET description = ?, annotations = ?, present = 1, last_seen = ? WHERE server_id = ? AND name = ?')
          .run(t.description, JSON.stringify(t.annotations), now, id, t.name);
      } else {
        this.#db
          .prepare('INSERT INTO mcp_tools (server_id, name, description, annotations, status, present, first_seen, last_seen) VALUES (?, ?, ?, ?, ?, 1, ?, ?)')
          .run(id, t.name, t.description, JSON.stringify(t.annotations), baseline ? 'baseline' : 'pending', now, now);
      }
    }
    for (const name of known.keys()) {
      if (!seen.has(name)) this.#db.prepare('UPDATE mcp_tools SET present = 0 WHERE server_id = ? AND name = ?').run(id, name);
    }
  }

  /** 	oken is the administrator's identity-provider access token, presented during discovery (Tool Discovery Snapshot). */
  async create(actor: Actor, body: Record<string, unknown>, token: string | null) {
    const e = this.validate(body);
    const groups = this.#groups(body.groups) ?? [];
    if (this.#row(e.id)) throw new ApiError(409, 'CONFLICT', `A catalog entry ${e.id} already exists.`);
    const wantEnabled = body.enabled === undefined ? true : body.enabled === true;
    const discovery = e.kind === 'remote' ? await this.#discover(e, token) : null;
    tx(this.#db, () => {
      this.#db
        .prepare('INSERT INTO mcp_servers (id, entry, enabled, discovery_status, updated_by, updated_at) VALUES (?, ?, 0, ?, ?, ?)')
        .run(e.id, JSON.stringify(e), discovery ? 'failed' : 'none', actor.userName, nowIso());
      for (const g of groups) this.#db.prepare('INSERT INTO mcp_assignments (server_id, workbench_group) VALUES (?, ?)').run(e.id, g);
      if (discovery) this.#storeDiscovery(e.id, discovery);
      if (wantEnabled && (!discovery || discovery.ok)) this.#db.prepare('UPDATE mcp_servers SET enabled = 1 WHERE id = ?').run(e.id);
    });
    this.#changed(actor, e.id, 'mcp.server.create', null, []);
    return this.get(e.id);
  }

  async update(actor: Actor, id: string, body: Record<string, unknown>, token: string | null) {
    const row = this.#row(id);
    if (!row) throw new ApiError(404, 'NOT_FOUND', `No catalog entry ${id}.`);
    const before = this.#view(row);
    const { groups: _g, enabled: _en, discovery: _d, tools: _t, ...current } = before;
    const e = this.validate({ ...current, ...body, id });
    const groups = this.#groups(body.groups);
    const wantEnabled = body.enabled === undefined ? before.enabled : body.enabled === true;
    const discovery = e.kind === 'remote' ? await this.#discover(e, token) : null;
    if (wantEnabled && discovery && !discovery.ok && body.enabled === true) {
      // Enabling requires a successful discovery; store the failure and refuse.
      this.#storeDiscovery(id, discovery);
      this.#changed(actor, id, 'mcp.server.discover', before, before.groups);
      throw new ApiError(409, 'DISCOVERY_FAILED', `The entry cannot be enabled until a discovery succeeds: ${discovery.error}`, { reason: discovery.error });
    }
    tx(this.#db, () => {
      this.#db.prepare('UPDATE mcp_servers SET entry = ?, updated_by = ?, updated_at = ? WHERE id = ?').run(JSON.stringify(e), actor.userName, nowIso(), id);
      if (e.kind === 'local') this.#db.prepare("UPDATE mcp_servers SET discovery_status = 'none', discovery_error = NULL WHERE id = ?").run(id);
      if (groups) {
        this.#db.prepare('DELETE FROM mcp_assignments WHERE server_id = ?').run(id);
        for (const g of groups) this.#db.prepare('INSERT INTO mcp_assignments (server_id, workbench_group) VALUES (?, ?)').run(id, g);
      }
      if (discovery) this.#storeDiscovery(id, discovery);
      const discoveredOk = !discovery || discovery.ok;
      this.#db.prepare('UPDATE mcp_servers SET enabled = ? WHERE id = ?').run(wantEnabled && discoveredOk ? 1 : 0, id);
    });
    this.#changed(actor, id, 'mcp.server.update', before, before.groups);
    return this.get(id);
  }

  async rediscover(actor: Actor, id: string, token: string | null) {
    const row = this.#row(id);
    if (!row) throw new ApiError(404, 'NOT_FOUND', `No catalog entry ${id}.`);
    const e = JSON.parse(row.entry) as Entry;
    if (e.kind !== 'remote') throw new ApiError(400, 'INVALID_REQUEST', 'A local entry cannot be discovered by the server.');
    const before = this.#view(row);
    this.#storeDiscovery(id, await this.#discover(e, token));
    this.#changed(actor, id, 'mcp.server.discover', before, before.groups);
    return this.get(id);
  }

  remove(actor: Actor, id: string) {
    const row = this.#row(id);
    if (!row) throw new ApiError(404, 'NOT_FOUND', `No catalog entry ${id}.`);
    const before = this.#view(row);
    this.#db.prepare('DELETE FROM mcp_servers WHERE id = ?').run(id);
    this.#changed(actor, id, 'mcp.server.remove', before, before.groups);
    return { id, removed: true };
  }

  /** Tool Review Decisions: include or exclude a discovered tool. */
  decideTool(actor: Actor, id: string, tool: string, decision: unknown) {
    if (decision !== 'included' && decision !== 'excluded') throw invalid('decision', 'decision must be included or excluded.');
    const row = this.#row(id);
    if (!row) throw new ApiError(404, 'NOT_FOUND', `No catalog entry ${id}.`);
    const before = this.#view(row);
    const r = this.#db
      .prepare('UPDATE mcp_tools SET status = ?, decided_by = ?, decided_at = ? WHERE server_id = ? AND name = ?')
      .run(decision, actor.userName, nowIso(), id, tool);
    if (r.changes === 0) throw new ApiError(404, 'NOT_FOUND', `The entry ${id} has no discovered tool ${tool}.`);
    this.#changed(actor, id, decision === 'included' ? 'mcp.tool.include' : 'mcp.tool.exclude', before, before.groups);
    return this.get(id);
  }

  // ---- packages -------------------------------------------------------------------------------------------------------

  uploadPackage(actor: Actor, bytes: Buffer, name: string) {
    if (bytes.length === 0) throw new ApiError(400, 'INVALID_REQUEST', 'The package is empty.');
    if (bytes.length > this.#cfg.mcpCatalog.maxPackageBytes) {
      throw new ApiError(413, 'TOO_LARGE', `The package is larger than the limit of ${this.#cfg.mcpCatalog.maxPackageBytes} bytes.`);
    }
    const sha256 = createHash('sha256').update(bytes).digest('hex');
    this.#db
      .prepare('INSERT OR IGNORE INTO mcp_packages (sha256, size, name, bytes, uploaded_by, uploaded_at) VALUES (?, ?, ?, ?, ?, ?)')
      .run(sha256, bytes.length, name, bytes, actor.userName, nowIso());
    this.#audit.write({ category: 'admin', action: 'mcp.package.upload', outcome: 'ok', subject: actor.subject, userName: actor.userName, details: { sha256, size: bytes.length, name } });
    return { sha256, size: bytes.length, name };
  }

  listPackages() {
    return this.#db.prepare('SELECT sha256, size, name, uploaded_by, uploaded_at FROM mcp_packages ORDER BY uploaded_at DESC').all();
  }

  /** Package Upload And Access: the bytes, only for a user whose Groups have an enabled entry naming the hash. */
  packageFor(groups: string[], sha256: string): Buffer | null {
    const sha = sha256.toLowerCase();
    if (!SHA256.test(sha)) return null;
    const allowed = this.#enabledFor(groups).some((r) => (JSON.parse(r.entry) as Entry).package === sha);
    if (!allowed) return null;
    const row = this.#db.prepare('SELECT bytes FROM mcp_packages WHERE sha256 = ?').get(sha) as { bytes: Uint8Array } | undefined;
    return row ? Buffer.from(row.bytes) : null;
  }
}
