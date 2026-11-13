// control-plane/api-connectors: documents, operation selection and risk, connection settings and secrets, personal access
// tokens, the bundle view of an API connector, and call execution with its audit (design D4 to D11).
import { lookup } from 'node:dns/promises';
import type { Config } from '../config.ts';
import { decrypt, encrypt, sanitizeUpstreamText } from '../gateway/connections.ts';
import type { Db } from '../db.ts';
import { nowIso, tx } from '../db.ts';
import { ApiError } from '../http.ts';
import type { Audit } from '../identity/audit.ts';
import type { McpCatalog } from '../mcp/catalog.ts';
import { RISK_CLASSES } from '../mcp/catalog.ts';
import { checkSchemaBudget } from './budget.ts';
import { readDocument, parseOpenApi } from './openapi.ts';
import { RISK_OF_METHOD, analyzeDocument, apiDescriptionProblem, MAX_SELECTED, type OperationReport } from './rules.ts';
import { isLinkLocalOrMetadata, validateApiSettings } from './settings.ts';

export const MAX_ARGUMENT_BYTES = 64 * 1024;
export const MAX_ERROR_TEXT_BYTES = 2 * 1024;
export const MAX_TOKEN_CHARS = 4096;

export interface Caller {
  subject: string;
  userName: string;
  groups: string[];
}

interface EntryRow {
  id: string;
  entry: string;
  enabled: number;
}

interface OpRow {
  connector_id: string;
  op_key: string;
  method: string;
  path: string;
  tool: string | null;
  description: string;
  usable: number;
  reason: string | null;
  input_schema: string | null;
  schema_bytes: number;
  call_spec: string | null;
  status: 'new' | 'unselected' | 'selected' | 'gone';
  risk: string;
  risk_reason: string | null;
}

export interface CallResult {
  status: number;
  body: unknown;
  truncated: boolean;
}

const invalid = (field: string, message: string) => new ApiError(400, 'VALIDATION_FAILED', message, { field });

/** Reads a response body up to `limit` bytes; the rest is dropped and `truncated` says so. */
async function readLimited(res: Response, limit: number): Promise<{ bytes: Buffer; truncated: boolean }> {
  const reader = res.body?.getReader();
  if (!reader) return { bytes: Buffer.alloc(0), truncated: false };
  const chunks: Buffer[] = [];
  let total = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    const chunk = Buffer.from(value);
    if (total + chunk.length > limit) {
      chunks.push(chunk.subarray(0, Math.max(0, limit - total)));
      await reader.cancel().catch(() => {});
      return { bytes: Buffer.concat(chunks), truncated: true };
    }
    total += chunk.length;
    chunks.push(chunk);
  }
  return { bytes: Buffer.concat(chunks), truncated: false };
}

/** Arguments against the operation's input schema: known names, required names and top-level types (Connector Call Authorization). */
export function checkArguments(schema: Record<string, unknown>, args: Record<string, unknown>): void {
  const props = (schema.properties ?? {}) as Record<string, Record<string, unknown>>;
  for (const k of Object.keys(args)) {
    if (!(k in props)) throw invalid(k, `The argument "${k}" is not part of this operation.`);
  }
  for (const r of (schema.required ?? []) as string[]) {
    if (args[r] === undefined) throw invalid(r, `The argument "${r}" is required.`);
  }
  for (const [k, v] of Object.entries(args)) {
    const p = props[k]!;
    if (v === undefined) continue;
    const enumOk = !Array.isArray(p.enum) || (p.enum as unknown[]).includes(v);
    const t = p.type as string | undefined;
    const typeOk =
      t === undefined ||
      (t === 'string' && typeof v === 'string') ||
      (t === 'integer' && Number.isInteger(v)) ||
      (t === 'number' && typeof v === 'number' && Number.isFinite(v)) ||
      (t === 'boolean' && typeof v === 'boolean') ||
      (t === 'array' && Array.isArray(v)) ||
      (t === 'object' && v !== null && typeof v === 'object' && !Array.isArray(v)) ||
      (t === 'null' && v === null);
    if (!typeOk || !enumOk) throw invalid(k, `The argument "${k}" must be ${t ? `a ${t}` : 'one of the allowed values'}.`);
  }
}

export class ApiConnectors {
  readonly #cfg: Config;
  readonly #db: Db;
  readonly #audit: Audit;
  readonly #catalog: McpCatalog;
  readonly #hooks: { onChanged: (groups: string[] | 'everyone') => void; secretKey: () => Buffer | null };
  readonly #inflight = new Map<string, number>();

  constructor(
    cfg: Config,
    db: Db,
    audit: Audit,
    catalog: McpCatalog,
    hooks: { onChanged: (groups: string[] | 'everyone') => void; secretKey: () => Buffer | null },
  ) {
    this.#cfg = cfg;
    this.#db = db;
    this.#audit = audit;
    this.#catalog = catalog;
    this.#hooks = hooks;
  }

  // ---- helpers --------------------------------------------------------------------------------------------------------

  #apiRow(id: string): { row: EntryRow; entry: Record<string, any> } {
    const row = this.#db.prepare('SELECT id, entry, enabled FROM mcp_servers WHERE id = ?').get(id) as EntryRow | undefined;
    if (!row) throw new ApiError(404, 'NOT_FOUND', `No catalog entry ${id}.`);
    const entry = JSON.parse(row.entry) as Record<string, any>;
    if (entry.kind !== 'api') throw new ApiError(400, 'INVALID_REQUEST', `${id} is not an API connector.`);
    return { row, entry };
  }

  /** An enabled entry assigned to one of the caller's Groups, or to everyone (control-plane/mcp-catalog Entry Delivered). */
  #visible(id: string, groups: string[]): EntryRow | undefined {
    const gs = [...new Set(['everyone', ...groups])];
    return this.#db
      .prepare(
        `SELECT DISTINCT s.id, s.entry, s.enabled FROM mcp_servers s JOIN mcp_assignments a ON a.server_id = s.id
         WHERE s.id = ? AND s.enabled = 1 AND a.skowork_group IN (${gs.map(() => '?').join(',')})`,
      )
      .get(id, ...gs) as EntryRow | undefined;
  }

  #ops(id: string): OpRow[] {
    return this.#db.prepare('SELECT * FROM api_operations WHERE connector_id = ? ORDER BY op_key').all(id) as unknown as OpRow[];
  }

  #key(): Buffer {
    const k = this.#hooks.secretKey();
    if (!k) throw new ApiError(409, 'NO_SECRET_KEY', 'SKOWORK_SECRET_KEY is not set, so no credential can be stored.');
    return k;
  }

  // ---- document -------------------------------------------------------------------------------------------------------

  /** API Document Upload, Operation Usability and Tool Selection: a new document flags new and changed operations. */
  uploadDocument(actor: Caller, id: string, text: string) {
    const { entry } = this.#apiRow(id);
    const parsed = parseOpenApi(readDocument(text, this.#cfg.apiConnectors.maxDocumentBytes) as Record<string, unknown>);
    const problem = apiDescriptionProblem(parsed.description, parsed.title);
    if (problem) throw invalid('info.description', problem);
    const { operations, problem: limit } = analyzeDocument(parsed);
    if (limit) throw invalid('paths', limit);

    const existing = new Map(this.#ops(id).map((r) => [r.op_key, r]));
    const later = existing.size > 0;
    const seen = new Set<string>();
    let flagged = 0;
    let gone = 0;
    const now = nowIso();
    tx(this.#db, () => {
      for (const op of operations as OperationReport[]) {
        seen.add(op.key);
        const prev = existing.get(op.key);
        const schema = op.inputSchema ? JSON.stringify(op.inputSchema) : null;
        const changed = !prev || prev.description !== op.description || prev.tool !== op.tool || prev.input_schema !== schema || Boolean(prev.usable) !== op.usable;
        // A new operation, or one that came back, or one whose description or schema changed, awaits review.
        let status: OpRow['status'] = prev ? prev.status : later ? 'new' : 'unselected';
        if (prev && prev.status !== 'new' && (prev.status === 'gone' || changed)) status = 'new';
        if (status === 'new') flagged++;
        const risk = prev && prev.status !== 'gone' ? prev.risk : (RISK_OF_METHOD[op.method.toLowerCase()] ?? 'read');
        this.#db
          .prepare(
            `INSERT INTO api_operations (connector_id, op_key, method, path, tool, description, usable, reason, input_schema, schema_bytes, call_spec, status, risk, risk_reason)
             VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
             ON CONFLICT (connector_id, op_key) DO UPDATE SET tool = excluded.tool, description = excluded.description, usable = excluded.usable,
               reason = excluded.reason, input_schema = excluded.input_schema, schema_bytes = excluded.schema_bytes, call_spec = excluded.call_spec,
               status = excluded.status, risk = excluded.risk`,
          )
          .run(
            id,
            op.key,
            op.method.toLowerCase(),
            op.path,
            op.tool,
            op.description,
            op.usable ? 1 : 0,
            op.reason,
            schema,
            op.schemaBytes,
            op.callSpec ? JSON.stringify(op.callSpec) : null,
            status,
            risk,
            prev?.risk_reason ?? null,
          );
      }
      for (const [key, prev] of existing) {
        if (seen.has(key) || prev.status === 'gone') continue;
        gone++;
        this.#db.prepare("UPDATE api_operations SET status = 'gone', usable = 0 WHERE connector_id = ? AND op_key = ?").run(id, key);
      }
      this.#db
        .prepare(
          `INSERT INTO api_documents (connector_id, title, openapi, uploaded_by, uploaded_at) VALUES (?, ?, ?, ?, ?)
           ON CONFLICT (connector_id) DO UPDATE SET title = excluded.title, openapi = excluded.openapi, uploaded_by = excluded.uploaded_by, uploaded_at = excluded.uploaded_at`,
        )
        .run(id, parsed.title, parsed.openapi, actor.userName, now);
    });
    const usable = operations.filter((o) => o.usable).length;
    this.#catalog.recordChange(actor, id, 'api.document.upload');
    this.#audit.write({
      category: 'admin', action: 'api.document.upload', outcome: 'ok', subject: actor.subject, userName: actor.userName,
      details: { id, name: entry.name, operations: operations.length, usable, unusable: operations.length - usable, flagged, gone },
    });
    return this.report(id);
  }

  /** The report of a connector: its document and every operation with its status and, when not usable, its reason. */
  report(id: string) {
    this.#apiRow(id);
    const doc = this.#db.prepare('SELECT title, openapi, uploaded_by, uploaded_at FROM api_documents WHERE connector_id = ?').get(id) as
      | { title: string; openapi: string; uploaded_by: string; uploaded_at: string }
      | undefined;
    const ops = this.#ops(id);
    return {
      document: doc ?? null,
      counts: {
        operations: ops.filter((o) => o.status !== 'gone').length,
        usable: ops.filter((o) => o.usable && o.status !== 'gone').length,
        selected: ops.filter((o) => o.status === 'selected').length,
        flagged: ops.filter((o) => o.status === 'new').length,
        gone: ops.filter((o) => o.status === 'gone').length,
      },
      operations: ops.map((o) => ({
        key: o.op_key,
        method: o.method.toUpperCase(),
        path: o.path,
        tool: o.tool,
        description: o.description,
        usable: o.usable === 1,
        reason: o.reason,
        status: o.status,
        risk: o.risk,
        risk_reason: o.risk_reason,
      })),
    };
  }

  // ---- selection and risk ---------------------------------------------------------------------------------------------

  /** Connector Selection And Risk: all changes are checked first; a refused save changes nothing. */
  saveSelection(actor: Caller, id: string, items: unknown) {
    this.#apiRow(id);
    if (!Array.isArray(items)) throw invalid('operations', 'operations must be a list.');
    const rows = new Map(this.#ops(id).map((r) => [r.op_key, { ...r }]));
    const touched = new Set<string>();
    for (const raw of items as Record<string, unknown>[]) {
      const key = typeof raw?.key === 'string' ? raw.key : '';
      const row = rows.get(key);
      if (!row) throw invalid('key', `There is no operation ${key || '(none)'} in this connector's document.`);
      if (typeof raw.selected !== 'boolean') throw invalid('selected', `selected of ${key} must be true or false.`);
      if (raw.selected) {
        if (row.status === 'gone') throw invalid('key', `${key} is no longer in the document, so it cannot be selected.`);
        if (!row.usable) throw invalid('key', `${key} cannot be selected: ${row.reason}.`);
        const risk = raw.risk ?? row.risk;
        if (typeof risk !== 'string' || !(RISK_CLASSES as readonly string[]).includes(risk)) throw invalid('risk', `The risk class of ${key} must be one of ${RISK_CLASSES.join(', ')}.`);
        const reason = typeof raw.risk_reason === 'string' ? raw.risk_reason.trim() : (row.risk_reason ?? '');
        if (risk === 'read' && row.method !== 'get' && !reason) {
          throw invalid('risk_reason', `${key} is not a GET operation; setting it to read needs a reason.`);
        }
        row.status = 'selected';
        row.risk = risk;
        row.risk_reason = risk === 'read' && row.method !== 'get' ? reason : null;
      } else if (row.status !== 'gone') {
        row.status = 'unselected';
      }
      rows.set(key, row);
      touched.add(key);
    }
    const selected = [...rows.values()].filter((r) => r.status === 'selected');
    if (selected.length > MAX_SELECTED) throw invalid('operations', `A connector offers at most ${MAX_SELECTED} tools; ${selected.length} are selected.`);
    const bytes = selected.reduce((n, r) => n + r.schema_bytes, 0);
    checkSchemaBudget(this.#db, id, this.#groupsOf(id), bytes);
    const now = nowIso();
    tx(this.#db, () => {
      for (const key of touched) {
        const r = rows.get(key)!;
        this.#db
          .prepare('UPDATE api_operations SET status = ?, risk = ?, risk_reason = ?, decided_by = ?, decided_at = ? WHERE connector_id = ? AND op_key = ?')
          .run(r.status, r.risk, r.risk_reason, actor.userName, now, id, key);
      }
    });
    this.#catalog.recordChange(actor, id, 'api.operations.save');
    this.#audit.write({
      category: 'admin', action: 'api.operations.save', outcome: 'ok', subject: actor.subject, userName: actor.userName,
      // The risk reason is part of the audit trail (Tool Risk Classes); the operation's description is not repeated here.
      details: { id, changed: [...touched].map((k) => ({ key: k, status: rows.get(k)!.status, risk: rows.get(k)!.risk, risk_reason: rows.get(k)!.risk_reason })) },
    });
    return this.report(id);
  }

  #groupsOf(id: string): string[] {
    return (this.#db.prepare('SELECT skowork_group FROM mcp_assignments WHERE server_id = ?').all(id) as { skowork_group: string }[]).map((r) => r.skowork_group);
  }

  // ---- settings and secrets -------------------------------------------------------------------------------------------

  /**
   * Creates an API connector entry with its settings and key. The settings are checked before anything is stored.
   * The key is written only after the catalog accepted the entry.
   */
  async createConnector(actor: Caller, body: Record<string, unknown>) {
    if (body.kind !== 'api') throw invalid('kind', 'An API connector needs kind api.');
    const validated = this.#settingsOf(body);
    const created = (await this.#catalog.create(actor, { ...body, kind: 'api' }, null)) as { id: string };
    this.#writeKey(actor, created.id, validated.auth, body);
    return this.adminView(created.id);
  }

  /** Changes the settings of an API connector; a new api-key replaces the stored one. */
  async saveSettings(actor: Caller, id: string, body: Record<string, unknown>) {
    const { entry } = this.#apiRow(id);
    const validated = this.#settingsOf({ ...entry, ...body, id });
    await this.#catalog.update(actor, id, body, null);
    this.#writeKey(actor, id, validated.auth, body);
    return this.adminView(id);
  }

  #settingsOf(body: Record<string, unknown>) {
    const s = validateApiSettings(body, this.#cfg.apiConnectors.allowLoopbackHttp);
    const key = typeof body.api_key === 'string' ? body.api_key.trim() : '';
    if (s.auth === 'api-key' && !key && !this.#hasKey(String(body.id ?? ''))) {
      throw invalid('api_key', 'An api-key connector needs its key.');
    }
    return s;
  }

  #hasKey(id: string): boolean {
    return Boolean(this.#db.prepare('SELECT 1 FROM api_connector_secrets WHERE connector_id = ?').get(id));
  }

  #writeKey(actor: Caller, id: string, auth: string, body: Record<string, unknown>): void {
    if (auth !== 'api-key') {
      const r = this.#db.prepare('DELETE FROM api_connector_secrets WHERE connector_id = ?').run(id);
      if (r.changes) this.#audit.write({ category: 'admin', action: 'api.key.remove', outcome: 'ok', subject: actor.subject, userName: actor.userName, details: { id } });
      return;
    }
    const key = typeof body.api_key === 'string' ? body.api_key.trim() : '';
    if (!key) return;
    const enc = encrypt(this.#key(), key);
    const hint = key.length > 8 ? key.slice(-4) : '';
    this.#db
      .prepare(
        `INSERT INTO api_connector_secrets (connector_id, key_enc, key_hint, set_by, set_at) VALUES (?, ?, ?, ?, ?)
         ON CONFLICT (connector_id) DO UPDATE SET key_enc = excluded.key_enc, key_hint = excluded.key_hint, set_by = excluded.set_by, set_at = excluded.set_at`,
      )
      .run(id, enc, hint, actor.userName, nowIso());
    // The audit row says that a key was set, never what it is.
    this.#audit.write({ category: 'admin', action: 'api.key.set', outcome: 'ok', subject: actor.subject, userName: actor.userName, details: { id } });
  }

  /** Removes a connector with its document and deletes every personal access token of it (Personal Access Token Removal). */
  removeConnector(actor: Caller, id: string) {
    this.#apiRow(id);
    const tokens = (this.#db.prepare('SELECT COUNT(*) AS n FROM api_connector_tokens WHERE connector_id = ?').get(id) as { n: number }).n;
    const result = this.#catalog.remove(actor, id);
    this.#audit.write({
      category: 'admin', action: 'api.tokens.removed', outcome: 'ok', subject: actor.subject, userName: actor.userName,
      details: { id, tokens_removed: tokens },
    });
    return result;
  }

  /** The admin's view: the entry, the report, whether a key is set (never the key), and how many users hold a token. */
  adminView(id: string) {
    const view = this.#catalog.get(id) as unknown as Record<string, unknown>;
    const key = this.#db.prepare('SELECT key_hint, set_at FROM api_connector_secrets WHERE connector_id = ?').get(id) as
      | { key_hint: string; set_at: string }
      | undefined;
    const holders = (this.#db.prepare('SELECT COUNT(*) AS n FROM api_connector_tokens WHERE connector_id = ?').get(id) as { n: number }).n;
    return { ...view, api: { ...this.report(id), key: key ? { set: true, set_at: key.set_at, hint: key.key_hint } : { set: false }, token_holders: holders } };
  }

  /** The Connectors list of the console: every entry, with the API summary where it applies. */
  adminList() {
    return this.#catalog.list().map((e) => {
      const entry = e as unknown as { id: string; kind: string };
      if (entry.kind !== 'api') return e;
      const r = this.report(entry.id);
      return { ...e, api: { counts: r.counts } };
    });
  }

  // ---- personal access tokens -----------------------------------------------------------------------------------------

  /** The connectors of the caller: kind, state for the caller, offered tools, and for user-pat the help text. */
  listFor(user: Caller) {
    const gs = [...new Set(['everyone', ...user.groups])];
    const rows = this.#db
      .prepare(
        `SELECT DISTINCT s.id, s.entry, s.enabled FROM mcp_servers s JOIN mcp_assignments a ON a.server_id = s.id
         WHERE s.enabled = 1 AND a.skowork_group IN (${gs.map(() => '?').join(',')}) ORDER BY s.id`,
      )
      .all(...gs) as unknown as EntryRow[];
    return {
      connectors: rows.map((r) => {
        const e = JSON.parse(r.entry) as Record<string, any>;
        if (e.kind !== 'api') {
          const tools = (this.#db.prepare("SELECT COUNT(*) AS n FROM mcp_tools WHERE server_id = ? AND status = 'included' AND present = 1").get(r.id) as { n: number }).n;
          return { id: r.id, name: e.name, kind: 'mcp', auth: e.auth ?? null, state: 'not-applicable', offered_tools: tools };
        }
        const state = this.#catalog.apiState(r.id, e.auth, user.userName);
        const offered = state === 'ready' || state === 'not-applicable' ? this.#catalog.selectedOperations(r.id).length : 0;
        const out: Record<string, unknown> = { id: r.id, name: e.name, kind: 'api', auth: e.auth, state, offered_tools: offered };
        if (e.auth === 'user-pat') {
          out.help_text = e.help_text;
          if (e.help_link) out.help_link = e.help_link;
          const t = this.#db.prepare('SELECT set_at FROM api_connector_tokens WHERE connector_id = ? AND user_name = ? COLLATE NOCASE').get(r.id, user.userName) as
            | { set_at: string }
            | undefined;
          if (t) out.token = { set: true, set_at: t.set_at };
        }
        return out;
      }),
    };
  }

  /** Sets or replaces the caller's token. The token is encrypted at once and never returned. */
  setToken(user: Caller, id: string, token: unknown) {
    const row = this.#visible(id, user.groups);
    if (!row) throw new ApiError(404, 'NOT_FOUND', `No connector ${id} in your catalog.`);
    const e = JSON.parse(row.entry) as Record<string, any>;
    if (e.kind !== 'api' || e.auth !== 'user-pat') throw invalid('id', 'This connector does not take a personal access token.');
    const value = typeof token === 'string' ? token.trim() : '';
    if (!value) throw invalid('token', 'token is required.');
    if (value.length > MAX_TOKEN_CHARS) throw invalid('token', `A token is at most ${MAX_TOKEN_CHARS} characters.`);
    const enc = encrypt(this.#key(), value);
    const now = nowIso();
    const existed = Boolean(
      this.#db.prepare('SELECT 1 FROM api_connector_tokens WHERE connector_id = ? AND user_name = ? COLLATE NOCASE').get(id, user.userName),
    );
    this.#db
      .prepare(
        `INSERT INTO api_connector_tokens (connector_id, user_name, token_enc, set_at, rejected_at) VALUES (?, ?, ?, ?, NULL)
         ON CONFLICT (connector_id, user_name) DO UPDATE SET token_enc = excluded.token_enc, set_at = excluded.set_at, rejected_at = NULL`,
      )
      .run(id, user.userName, enc, now);
    this.#audit.write({
      category: 'api', action: 'api.token.set', outcome: 'ok', subject: user.subject, userName: user.userName,
      details: { connector: id, replaced: existed },
    });
    this.#changedFor(user.groups);
    return { id, state: 'ready', token: { set: true, set_at: now } };
  }

  /** Removes the caller's token. */
  removeToken(user: Caller, id: string) {
    const row = this.#visible(id, user.groups);
    if (!row) throw new ApiError(404, 'NOT_FOUND', `No connector ${id} in your catalog.`);
    const r = this.#db.prepare('DELETE FROM api_connector_tokens WHERE connector_id = ? AND user_name = ? COLLATE NOCASE').run(id, user.userName);
    this.#audit.write({
      category: 'api', action: 'api.token.remove', outcome: 'ok', subject: user.subject, userName: user.userName,
      details: { connector: id, removed: Number(r.changes) },
    });
    this.#changedFor(user.groups);
    return { id, state: 'needs-token', token: null };
  }

  #rejectToken(user: Caller, id: string, status: number): void {
    this.#db
      .prepare('UPDATE api_connector_tokens SET rejected_at = ? WHERE connector_id = ? AND user_name = ? COLLATE NOCASE AND rejected_at IS NULL')
      .run(nowIso(), id, user.userName);
    this.#audit.write({
      category: 'api', action: 'api.token.rejected', outcome: 'refused', subject: user.subject, userName: user.userName,
      details: { connector: id, upstream_status: status },
    });
    this.#changedFor(user.groups);
  }

  #changedFor(groups: string[]): void {
    this.#catalog.invalidate();
    this.#hooks.onChanged(groups);
  }

  // ---- calls ----------------------------------------------------------------------------------------------------------

  /**
   * Connector Call Authorization, Upstream Request Rules, Connector Concurrency, Response Limits and Connector Call Audit.
   * `bearer` is the caller's identity-provider token, used only for user-token connectors.
   */
  async call(user: Caller, bearer: string | null, id: string, tool: string, args: unknown): Promise<CallResult> {
    const started = Date.now();
    const argNames = args && typeof args === 'object' && !Array.isArray(args) ? Object.keys(args as object) : [];
    let status: number | null = null;
    let responseBytes = 0;
    try {
      const out = await this.#call(user, bearer, id, tool, args, (s, bytes) => {
        status = s;
        responseBytes = bytes;
      });
      this.#auditCall(user, id, tool, 'ok', { status, duration_ms: Date.now() - started, response_bytes: responseBytes, arguments: argNames });
      return out;
    } catch (err) {
      const reason = err instanceof ApiError ? err.message : 'the call failed';
      this.#auditCall(user, id, tool, err instanceof ApiError && err.status < 500 ? 'refused' : 'error', {
        status, duration_ms: Date.now() - started, response_bytes: responseBytes, arguments: argNames, reason,
      });
      throw err;
    }
  }

  #auditCall(user: Caller, id: string, tool: string, outcome: 'ok' | 'refused' | 'error', details: Record<string, unknown>): void {
    this.#audit.write({
      category: 'api', action: 'api.call', outcome, subject: user.subject, userName: user.userName,
      reason: typeof details.reason === 'string' ? details.reason : null,
      details: { connector: id, operation: tool, ...details, reason: undefined },
    });
  }

  async #call(
    user: Caller,
    bearer: string | null,
    id: string,
    tool: string,
    args: unknown,
    done: (status: number, bytes: number) => void,
  ): Promise<CallResult> {
    const row = this.#visible(id, user.groups);
    if (!row) throw new ApiError(403, 'NOT_OFFERED', 'This connector is not in your catalog.', { reason: 'not in the user\'s catalog' });
    const entry = JSON.parse(row.entry) as Record<string, any>;
    if (entry.kind !== 'api') throw new ApiError(400, 'INVALID_REQUEST', `${id} is not an API connector.`);
    const op = this.#db
      .prepare("SELECT * FROM api_operations WHERE connector_id = ? AND tool = ? AND status = 'selected' AND usable = 1")
      .get(id, tool) as OpRow | undefined;
    if (!op) {
      throw new ApiError(403, 'NOT_OFFERED', `The operation ${tool} is not offered by this connector.`, { reason: 'not selected or not usable' });
    }
    const state = this.#catalog.apiState(id, entry.auth, user.userName);
    if (state === 'needs-token') throw new ApiError(403, 'TOKEN_MISSING', 'Add your token for this connector on the Connectors page.', { reason: 'token missing' });
    if (state === 'token-rejected') throw new ApiError(403, 'TOKEN_REJECTED', 'Token rejected, replace it.', { reason: 'token rejected' });

    const input = args === undefined || args === null ? {} : args;
    if (typeof input !== 'object' || Array.isArray(input)) throw invalid('arguments', 'arguments must be an object.');
    if (Buffer.byteLength(JSON.stringify(input), 'utf8') > MAX_ARGUMENT_BYTES) {
      throw new ApiError(413, 'TOO_LARGE', `The arguments are larger than the limit of ${MAX_ARGUMENT_BYTES} bytes.`);
    }
    const a = input as Record<string, unknown>;
    const schema = JSON.parse(op.input_schema!) as Record<string, unknown>;
    checkArguments(schema, a);
    const spec = JSON.parse(op.call_spec!) as { method: string; path: string; pathParams: string[]; queryParams: string[]; body: boolean };
    for (const p of (schema.required ?? []) as string[]) if (a[p] === undefined) throw invalid(p, `The argument "${p}" is required.`);

    // Path values: no "." or ".." segments and no absolute address, so the call stays under the base URL (Egress For API Connectors).
    const base = new URL(entry.base_url as string);
    let pathPart = spec.path;
    for (const name of spec.pathParams) {
      const v = String(a[name]);
      if (v === '.' || v === '..') throw invalid(name, `The path value of ${name} cannot be "${v}".`);
      if (/^[a-z][a-z0-9+.-]*:/i.test(v) || v.startsWith('//')) throw new ApiError(403, 'DENIED', `The value of ${name} names another address and is refused.`, { reason: 'other host' });
      pathPart = pathPart.replace(`{${name}}`, encodeURIComponent(v));
    }
    const url = new URL(base.pathname.replace(/\/+$/, '') + pathPart, base.origin);
    for (const name of spec.queryParams) {
      const v = a[name];
      if (v === undefined) continue;
      for (const item of Array.isArray(v) ? v : [v]) url.searchParams.append(name, typeof item === 'object' ? JSON.stringify(item) : String(item));
    }
    if (url.origin !== base.origin || !url.pathname.startsWith(base.pathname.replace(/\/+$/, ''))) {
      throw new ApiError(403, 'DENIED', 'The call would leave the connector\'s base URL and is refused.', { reason: 'outside base URL' });
    }

    const headers: Record<string, string> = { accept: 'application/json' };
    // The credential values of this call; upstream error text is sanitized against them.
    const secrets: string[] = [];
    if (entry.auth === 'api-key') {
      const key = this.#secretOf(id);
      if (!key) throw new ApiError(409, 'NO_KEY', 'This connector has no key stored.');
      headers[entry.key_header as string] = key;
      secrets.push(key);
    } else if (entry.auth === 'user-token') {
      if (!bearer) throw new ApiError(401, 'UNAUTHORIZED', 'The call needs your sign-in.');
      headers.authorization = `Bearer ${bearer}`;
      secrets.push(bearer);
    } else if (entry.auth === 'user-pat') {
      const token = this.#tokenOf(id, user.userName);
      headers[entry.key_header as string] = entry.key_scheme ? `${entry.key_scheme} ${token}` : token;
      secrets.push(token);
    }
    let body: string | undefined;
    if (spec.body && a.body !== undefined) {
      body = JSON.stringify(a.body);
      headers['content-type'] = 'application/json';
    }

    const busy = this.#inflight.get(id) ?? 0;
    if (busy >= this.#cfg.apiConnectors.maxConcurrentCalls) {
      throw new ApiError(429, 'BUSY', 'The connector is busy with other calls; the call was not made. Ask again later.', { reason: 'busy' });
    }
    this.#inflight.set(id, busy + 1);
    try {
      await this.#checkAddress(url.hostname);
      let res: Response;
      try {
        res = await fetch(url, {
          method: spec.method.toUpperCase(),
          headers,
          body,
          redirect: 'manual',
          signal: AbortSignal.timeout(this.#cfg.apiConnectors.callTimeoutMs),
        });
      } catch (err) {
        const timeout = (err as Error).name === 'TimeoutError' || (err as Error).name === 'AbortError';
        throw new ApiError(timeout ? 504 : 502, timeout ? 'TIMEOUT' : 'UPSTREAM_ERROR', timeout ? 'The API did not answer in time.' : 'The API could not be reached.', {
          reason: timeout ? 'timeout' : 'unreachable',
        });
      }
      done(res.status, 0);
      if (res.status >= 300 && res.status < 400) {
        await res.body?.cancel().catch(() => {});
        throw new ApiError(502, 'UPSTREAM_ERROR', `The API answered with a redirect (${res.status}); redirects are not followed.`, { status: res.status, reason: 'redirect' });
      }
      // Only a 401 rejects the user's token. A 403 refuses this operation for the account and leaves the token ready.
      if (entry.auth === 'user-pat' && res.status === 401) {
        await res.body?.cancel().catch(() => {});
        this.#rejectToken(user, id, res.status);
        throw new ApiError(403, 'TOKEN_REJECTED', 'Token rejected, replace it.', { status: res.status, reason: 'token rejected' });
      }
      if (res.status < 200 || res.status >= 300) {
        const text = await readLimited(res, MAX_ERROR_TEXT_BYTES);
        const sanitized = sanitizeUpstreamText(text.bytes.toString('utf8'), secrets);
        if (res.status === 403) {
          throw new ApiError(403, 'FORBIDDEN', 'The API refused this operation for your account (403).', {
            status: 403,
            text: sanitized,
            reason: 'forbidden operation',
          });
        }
        throw new ApiError(502, 'UPSTREAM_ERROR', `The API answered ${res.status}.`, {
          status: res.status,
          text: sanitized,
          reason: 'upstream status',
        });
      }
      const type = res.headers.get('content-type') ?? '';
      const isJson = /^application\/(.+\+)?json\b/i.test(type);
      const isText = /^text\//i.test(type);
      if (!isJson && !isText) {
        await res.body?.cancel().catch(() => {});
        throw new ApiError(415, 'UNSUPPORTED_CONTENT', 'Binary responses are not supported.', { reason: 'binary response' });
      }
      const limit = this.#cfg.apiConnectors.maxResponseBytes;
      const { bytes, truncated } = await readLimited(res, limit);
      done(res.status, bytes.length);
      const text = bytes.toString('utf8');
      let value: unknown = text;
      if (isJson && !truncated) {
        try {
          value = JSON.parse(text);
        } catch {
          value = text;
        }
      }
      return { status: res.status, body: value, truncated };
    } finally {
      const left = (this.#inflight.get(id) ?? 1) - 1;
      if (left <= 0) this.#inflight.delete(id);
      else this.#inflight.set(id, left);
    }
  }

  /** Refuses link-local and metadata addresses, whatever the name resolves to (Upstream Request Rules). */
  async #checkAddress(host: string): Promise<void> {
    const addresses = await lookup(host.replace(/^\[|\]$/g, ''), { all: true }).catch(() => []);
    for (const a of addresses) {
      if (isLinkLocalOrMetadata(a.address)) throw new ApiError(403, 'DENIED', 'The address of the connector is a link-local or metadata address and is refused.', { reason: 'address refused' });
    }
  }

  #secretOf(id: string): string | null {
    const r = this.#db.prepare('SELECT key_enc FROM api_connector_secrets WHERE connector_id = ?').get(id) as { key_enc: string } | undefined;
    if (!r) return null;
    const k = this.#hooks.secretKey();
    if (!k) return null;
    return decrypt(k, r.key_enc);
  }

  #tokenOf(id: string, userName: string): string {
    const r = this.#db.prepare('SELECT token_enc FROM api_connector_tokens WHERE connector_id = ? AND user_name = ? COLLATE NOCASE').get(id, userName) as
      | { token_enc: string }
      | undefined;
    const k = this.#hooks.secretKey();
    if (!r || !k) throw new ApiError(403, 'TOKEN_MISSING', 'Add your token for this connector on the Connectors page.', { reason: 'token missing' });
    return decrypt(k, r.token_enc);
  }
}
