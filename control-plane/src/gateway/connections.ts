// LLM connections of control-plane/llm-gateway (add-llm-connections): stored OpenAI-compatible upstreams with
// encrypted credentials (D1), the environment bootstrap (D2), routing of model ids to connections (D3) and model
// discovery (D4). Credentials never leave this module except in the Authorization header sent to the upstream.
import { createCipheriv, createDecipheriv, randomBytes, scryptSync } from 'node:crypto';
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import type { Config, ModelCapability } from '../config.ts';
import type { Db } from '../db.ts';
import { nowIso, tx } from '../db.ts';
import { ApiError } from '../http.ts';
import type { Audit } from '../identity/audit.ts';
import type { User } from '../identity/identity.ts';

const DISCOVERY_TIMEOUT_MS = 10_000;
const KINDS = ['chat', 'embed', 'rerank', 'image'] as const;
const NAME = /^[a-z0-9][a-z0-9-]{0,39}$/;

export class Backend {
  readonly id: number;
  name: string;
  baseUrl: string;
  apiKey: string;
  maxConcurrent: number;
  inFlight = 0;
  healthy = true;
  constructor(id: number, name: string, baseUrl: string, apiKey: string, maxConcurrent: number) {
    this.id = id;
    this.name = name;
    this.baseUrl = baseUrl.replace(/\/+$/, '');
    this.apiKey = apiKey;
    this.maxConcurrent = maxConcurrent;
  }
  endpoint(path: string) {
    return `${this.baseUrl}/${path}`;
  }
  toString() {
    return `Backend(${this.name})`; // never the address or the key
  }
}

interface ConnectionRow {
  id: number;
  name: string;
  base_url: string;
  credential_enc: string | null;
  credential_hint: string | null;
  enabled: number;
  max_concurrent: number;
  position: number;
  status: string;
  status_detail: string | null;
  checked_at: string | null;
}

interface ModelRow {
  connection_id: number;
  model_id: string;
  kind: string;
  context_window: number;
  max_output_tokens: number | null;
  tools: number;
  images: number;
  reasoning: number;
  dimension: number | null;
  last_seen: string | null;
  missing: number;
}

// ---- credential encryption (D1) ----------------------------------------------------------------------

/**
 * The server key from WORKBENCH_SECRET_KEY (Server Key From A Passphrase): the base64 encoding of 32 bytes is the key
 * itself (keys of earlier deployments); any other value of at least 16 characters is a passphrase the key is derived
 * from. The salt is fixed so the same passphrase gives the same key after a restart.
 */
export function serverKey(value: string): Buffer {
  const v = value.trim();
  if (/^[A-Za-z0-9+/]{43}=$/.test(v)) {
    const k = Buffer.from(v, 'base64');
    if (k.length === 32) return k;
  }
  if (v.length < 16) throw new StartupError('WORKBENCH_SECRET_KEY must be at least 16 characters (any passphrase, for example four random words)');
  return scryptSync(v, 'workbench/llm-credentials/v1', 32, { N: 2 ** 15, r: 8, p: 1, maxmem: 64 * 1024 * 1024 });
}

/** A settings problem that stops the control plane at startup; main.ts prints only its message. */
export class StartupError extends Error {}

function loadKey(cfg: Config, needed: boolean): Buffer | null {
  if (cfg.gateway.secretKey) return serverKey(cfg.gateway.secretKey);
  if (cfg.env === 'production') {
    if (needed) throw new StartupError('WORKBENCH_SECRET_KEY is required: LLM connection credentials are stored encrypted with it (any passphrase of at least 16 characters)');
    return null;
  }
  if (cfg.dbPath === ':memory:') return randomBytes(32);
  // Development: a key file next to the database, readable by its owner only.
  const file = join(dirname(cfg.dbPath), 'secret.key');
  if (existsSync(file)) return Buffer.from(readFileSync(file, 'utf8').trim(), 'base64');
  const k = randomBytes(32);
  writeFileSync(file, k.toString('base64'), { mode: 0o600 });
  return k;
}

function encrypt(key: Buffer, plain: string): string {
  const iv = randomBytes(12);
  const c = createCipheriv('aes-256-gcm', key, iv);
  const ct = Buffer.concat([c.update(plain, 'utf8'), c.final()]);
  return 'v1.' + Buffer.concat([iv, c.getAuthTag(), ct]).toString('base64');
}

function decrypt(key: Buffer, enc: string): string {
  const raw = Buffer.from(enc.replace(/^v1\./, ''), 'base64');
  const d = createDecipheriv('aes-256-gcm', key, raw.subarray(0, 12));
  d.setAuthTag(raw.subarray(12, 28));
  return Buffer.concat([d.update(raw.subarray(28)), d.final()]).toString('utf8');
}

const hint = (credential: string) => (credential.length > 8 ? credential.slice(-4) : '');

// ---- defaults for model ids ------------------------------------------------------------------------------------

function guessCapability(cfg: Config, id: string): ModelCapability {
  const known = cfg.gateway.capabilities[id];
  if (known) return known;
  if (/embed/i.test(id)) return { kind: 'embed', context_window: 8192 };
  if (/rerank/i.test(id)) return { kind: 'rerank', context_window: 8192 };
  if (/image|dall-e/i.test(id)) return { kind: 'image', context_window: 0 };
  return { kind: 'chat', context_window: 32768, tools: false };
}

const capabilityOf = (m: ModelRow): ModelCapability => ({
  kind: m.kind as ModelCapability['kind'],
  context_window: m.context_window,
  ...(m.max_output_tokens === null ? {} : { max_output_tokens: m.max_output_tokens }),
  tools: Boolean(m.tools),
  images: Boolean(m.images),
  reasoning: Boolean(m.reasoning),
  ...(m.dimension === null ? {} : { dimension: m.dimension }),
});

/** Where a role mapping is used in a policy in force: for the in-use check before a delete. */
export interface ModelUse {
  scope: string;
  role: string;
  model: string;
}

export class Connections {
  readonly #cfg: Config;
  readonly #db: Db;
  readonly #audit: Audit;
  readonly #log: (m: string) => void;
  #key: Buffer | null;
  readonly #backends = new Map<number, Backend>();
  /** model id -> connection ids offering it, in connection order (D3). */
  #offers = new Map<string, number[]>();
  #order: Backend[] = [];
  readonly #timer: NodeJS.Timeout;
  #uses: () => ModelUse[] = () => [];

  constructor(cfg: Config, db: Db, audit: Audit, log: (m: string) => void) {
    this.#cfg = cfg;
    this.#db = db;
    this.#audit = audit;
    this.#log = log;
    const stored = (db.prepare('SELECT COUNT(*) AS n FROM llm_connections').get() as { n: number }).n;
    this.#key = loadKey(cfg, stored > 0 || cfg.gateway.upstreams.length > 0);
    if (stored === 0) this.#bootstrap();
    this.#rebuild();
    // Discovery for connections never checked, then every interval (D4).
    for (const c of this.#rows()) if (c.enabled && !c.checked_at) void this.discover(c.id);
    this.#timer = setInterval(() => {
      for (const c of this.#rows()) if (c.enabled) void this.discover(c.id);
    }, cfg.gateway.discoveryIntervalMs);
    this.#timer.unref();
  }

  close() {
    clearInterval(this.#timer);
  }

  /** The role mappings in force, for the in-use check (set by the policy service). */
  setUseSource(fn: () => ModelUse[]) {
    this.#uses = fn;
  }

  // ---- bootstrap (D2) ----------------------------------------------------------------------------------------

  #bootstrap() {
    const ups = this.#cfg.gateway.upstreams;
    if (!ups.length) return;
    tx(this.#db, () => {
      ups.forEach((u, i) => {
        const id = Number(
          this.#db
            .prepare('INSERT INTO llm_connections (name, base_url, credential_enc, credential_hint, enabled, max_concurrent, position) VALUES (?, ?, ?, ?, 1, ?, ?)')
            .run(u.name, u.baseUrl, u.apiKey ? encrypt(this.#key!, u.apiKey) : null, u.apiKey ? hint(u.apiKey) : null, u.maxConcurrent, i).lastInsertRowid,
        );
        // The code defaults seed the known model ids, so routing works before the first discovery.
        for (const [model, cap] of Object.entries(this.#cfg.gateway.capabilities)) this.#upsertModel(id, model, cap, null);
      });
    });
    this.#audit.write({ category: 'admin', action: 'llm.connection.bootstrap', outcome: 'ok', details: { connections: ups.map((u) => u.name) } });
    this.#log(`LLM connections created from the environment: ${ups.map((u) => u.name).join(', ')}`);
  }

  #upsertModel(connectionId: number, model: string, cap: ModelCapability, lastSeen: string | null) {
    this.#db
      .prepare(
        `INSERT INTO llm_models (connection_id, model_id, kind, context_window, max_output_tokens, tools, images, reasoning, dimension, last_seen, missing)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0)
         ON CONFLICT (connection_id, model_id) DO UPDATE SET last_seen = COALESCE(excluded.last_seen, last_seen), missing = 0`,
      )
      .run(connectionId, model, cap.kind, cap.context_window, cap.max_output_tokens ?? null, cap.tools ? 1 : 0, cap.images ? 1 : 0, cap.reasoning ? 1 : 0, cap.dimension ?? null, lastSeen);
  }

  // ---- routing (D3) ------------------------------------------------------------------------------------------

  #rows(): ConnectionRow[] {
    return this.#db.prepare('SELECT * FROM llm_connections ORDER BY position, id').all() as unknown as ConnectionRow[];
  }

  #row(id: number): ConnectionRow {
    const r = this.#db.prepare('SELECT * FROM llm_connections WHERE id = ?').get(id) as ConnectionRow | undefined;
    if (!r) throw new ApiError(404, 'NOT_FOUND', 'No such connection.');
    return r;
  }

  /** Rebuilds the backends and the routing table after every change. */
  #rebuild() {
    const rows = this.#rows();
    const seen = new Set<number>();
    const order: Backend[] = [];
    for (const r of rows) {
      if (!r.enabled) continue;
      let key = '';
      if (r.credential_enc) {
        try {
          if (!this.#key) throw new Error('no key');
          key = decrypt(this.#key, r.credential_enc);
        } catch {
          this.#setStatus(r.id, 'failing', 'credential cannot be decrypted');
          continue;
        }
      }
      seen.add(r.id);
      const b = this.#backends.get(r.id);
      if (b) {
        b.name = r.name;
        b.baseUrl = r.base_url.replace(/\/+$/, '');
        b.apiKey = key;
        b.maxConcurrent = r.max_concurrent;
        order.push(b);
      } else {
        const nb = new Backend(r.id, r.name, r.base_url, key, r.max_concurrent);
        this.#backends.set(r.id, nb);
        order.push(nb);
      }
    }
    for (const id of [...this.#backends.keys()]) if (!seen.has(id)) this.#backends.delete(id);
    this.#order = order;
    const offers = new Map<string, number[]>();
    const models = this.#db.prepare('SELECT connection_id, model_id FROM llm_models').all() as { connection_id: number; model_id: string }[];
    for (const b of order) {
      for (const m of models) if (m.connection_id === b.id) offers.set(m.model_id, [...(offers.get(m.model_id) ?? []), b.id]);
    }
    this.#offers = offers;
  }

  /** The enabled, usable backends in connection order. */
  get backends(): Backend[] {
    return this.#order;
  }

  /** Splits `<connection>/<id>`: the prefix pins a connection only when a connection has that name. */
  #split(ref: string): { connection: string | null; model: string } {
    const i = ref.indexOf('/');
    if (i > 0) {
      const name = ref.slice(0, i);
      if (this.#db.prepare('SELECT 1 FROM llm_connections WHERE name = ?').get(name)) return { connection: name, model: ref.slice(i + 1) };
    }
    return { connection: null, model: ref };
  }

  /** The upstream model id and the enabled connections that offer it, in order; none when nothing resolves. */
  route(ref: string): { model: string; backends: Backend[] } {
    const { connection, model } = this.#split(ref);
    const ids = this.#offers.get(model) ?? [];
    const backends = ids.map((id) => this.#backends.get(id)!).filter((b) => b && (connection === null || b.name === connection));
    return { model, backends };
  }

  /** The stored capabilities of a mapped model (Model Capabilities From Connections). */
  capability(ref: string): ModelCapability {
    const { connection, model } = this.#split(ref);
    const row = this.#db
      .prepare(
        `SELECT m.* FROM llm_models m JOIN llm_connections c ON c.id = m.connection_id
         WHERE m.model_id = ? AND (? IS NULL OR c.name = ?) ORDER BY c.enabled DESC, c.position, c.id LIMIT 1`,
      )
      .get(model, connection, connection) as ModelRow | undefined;
    return row ? capabilityOf(row) : guessCapability(this.#cfg, model);
  }

  // ---- discovery (D4) ----------------------------------------------------------------------------------------

  #setStatus(id: number, status: string, detail: string | null) {
    this.#db.prepare('UPDATE llm_connections SET status = ?, status_detail = ?, checked_at = ? WHERE id = ?').run(status, detail, nowIso(), id);
  }

  /** Lists the connection's models; on failure the status says why and the earlier model list is kept. */
  async discover(id: number): Promise<void> {
    const r = this.#db.prepare('SELECT * FROM llm_connections WHERE id = ?').get(id) as ConnectionRow | undefined;
    if (!r) return;
    let key = '';
    try {
      if (r.credential_enc) key = decrypt(this.#key!, r.credential_enc);
    } catch {
      this.#setStatus(id, 'failing', 'credential cannot be decrypted');
      return;
    }
    this.#db.prepare("UPDATE llm_connections SET status = 'checking' WHERE id = ?").run(id);
    let ids: string[];
    try {
      const res = await fetch(`${r.base_url.replace(/\/+$/, '')}/models`, {
        headers: key ? { authorization: `Bearer ${key}` } : {},
        signal: AbortSignal.timeout(DISCOVERY_TIMEOUT_MS),
      });
      if (!res.ok) {
        await res.body?.cancel();
        this.#setStatus(id, 'failing', `HTTP ${res.status}`);
        return;
      }
      const body = (await res.json()) as { data?: { id?: unknown }[] };
      ids = (Array.isArray(body?.data) ? body.data : []).map((m) => m?.id).filter((m): m is string => typeof m === 'string' && m.length > 0 && m.length <= 200);
    } catch (err) {
      const name = (err as Error).name;
      this.#setStatus(id, 'failing', name === 'TimeoutError' ? 'no answer within 10 s' : name === 'SyntaxError' ? 'the model list is not JSON' : 'not reachable');
      return;
    }
    if (!this.#db.prepare('SELECT 1 FROM llm_connections WHERE id = ?').get(id)) return; // deleted meanwhile
    const now = nowIso();
    tx(this.#db, () => {
      for (const m of ids) this.#upsertModel(id, m, guessCapability(this.#cfg, m), now);
      const listed = new Set(ids);
      for (const m of this.#db.prepare('SELECT model_id FROM llm_models WHERE connection_id = ?').all(id) as { model_id: string }[]) {
        if (!listed.has(m.model_id)) this.#db.prepare('UPDATE llm_models SET missing = 1 WHERE connection_id = ? AND model_id = ?').run(id, m.model_id);
      }
    });
    this.#setStatus(id, 'ok', `${ids.length} models listed`);
    const b = this.#backends.get(id);
    if (b) b.healthy = true;
    this.#rebuild();
  }

  // ---- administration (1.3) ----------------------------------------------------------------------------------

  #view(r: ConnectionRow) {
    const models = (this.#db.prepare('SELECT * FROM llm_models WHERE connection_id = ? ORDER BY model_id').all(r.id) as unknown as ModelRow[]).map((m) => ({
      id: m.model_id,
      ...capabilityOf(m),
      last_seen: m.last_seen,
      missing: Boolean(m.missing),
    }));
    return {
      id: r.id,
      name: r.name,
      base_url: r.base_url,
      credential_set: Boolean(r.credential_enc),
      credential_hint: r.credential_hint ?? '',
      enabled: Boolean(r.enabled),
      max_concurrent: r.max_concurrent,
      position: r.position,
      status: r.status,
      status_detail: r.status_detail,
      checked_at: r.checked_at,
      models,
    };
  }

  list() {
    return { connections: this.#rows().map((r) => this.#view(r)), key_available: this.#key !== null };
  }

  get(id: number) {
    return this.#view(this.#row(id));
  }

  #record(a: User, action: string, details: Record<string, unknown>) {
    this.#audit.write({ category: 'admin', action, outcome: 'ok', subject: a.subject, userName: a.userName, details });
  }

  #validUrl(v: unknown): string {
    if (typeof v !== 'string' || !v.trim()) throw new ApiError(400, 'INVALID_REQUEST', 'base_url is required.');
    let u: URL;
    try {
      u = new URL(v.trim());
    } catch {
      throw new ApiError(400, 'INVALID_REQUEST', 'base_url is not a valid URL.');
    }
    const loopback = ['127.0.0.1', 'localhost', '[::1]'].includes(u.hostname);
    if (u.protocol !== 'https:' && !(u.protocol === 'http:' && loopback && this.#cfg.env !== 'production')) {
      throw new ApiError(400, 'INVALID_REQUEST', 'base_url must use https (http only for loopback in development).');
    }
    if (u.username || u.password || u.search || u.hash) throw new ApiError(400, 'INVALID_REQUEST', 'base_url must not carry credentials, a query or a fragment.');
    return u.toString().replace(/\/+$/, '');
  }

  #fields(body: Record<string, unknown>, partial: boolean) {
    const out: { name?: string; base_url?: string; credential?: string; enabled?: boolean; max_concurrent?: number } = {};
    if (body.name !== undefined || !partial) {
      if (typeof body.name !== 'string' || !NAME.test(body.name)) throw new ApiError(400, 'INVALID_REQUEST', 'name must be 1-40 lowercase letters, digits or dashes.');
      out.name = body.name;
    }
    if (body.base_url !== undefined || !partial) out.base_url = this.#validUrl(body.base_url);
    if (body.credential !== undefined) {
      if (typeof body.credential !== 'string' || body.credential.length > 4096) throw new ApiError(400, 'INVALID_REQUEST', 'credential must be a string.');
      out.credential = body.credential;
    }
    if (body.enabled !== undefined) {
      if (typeof body.enabled !== 'boolean') throw new ApiError(400, 'INVALID_REQUEST', 'enabled must be true or false.');
      out.enabled = body.enabled;
    }
    if (body.max_concurrent !== undefined) {
      if (!Number.isInteger(body.max_concurrent) || (body.max_concurrent as number) < 0 || (body.max_concurrent as number) > 10_000) {
        throw new ApiError(400, 'INVALID_REQUEST', 'max_concurrent must be an integer from 0 (no limit) to 10000.');
      }
      out.max_concurrent = body.max_concurrent as number;
    }
    return out;
  }

  #needKey(): Buffer {
    if (!this.#key) throw new ApiError(409, 'NO_SECRET_KEY', 'WORKBENCH_SECRET_KEY is not set, so no credential can be stored.');
    return this.#key;
  }

  async create(a: User, body: Record<string, unknown>) {
    const f = this.#fields(body, false);
    if (this.#db.prepare('SELECT 1 FROM llm_connections WHERE name = ?').get(f.name!)) throw new ApiError(409, 'CONFLICT', `A connection named ${f.name} exists.`);
    const enc = f.credential ? encrypt(this.#needKey(), f.credential) : null;
    const position = (this.#db.prepare('SELECT COALESCE(MAX(position), -1) + 1 AS p FROM llm_connections').get() as { p: number }).p;
    const id = Number(
      this.#db
        .prepare('INSERT INTO llm_connections (name, base_url, credential_enc, credential_hint, enabled, max_concurrent, position) VALUES (?, ?, ?, ?, ?, ?, ?)')
        .run(f.name!, f.base_url!, enc, f.credential ? hint(f.credential) : null, f.enabled === false ? 0 : 1, f.max_concurrent ?? 0, position).lastInsertRowid,
    );
    this.#record(a, 'llm.connection.add', { id, name: f.name, base_url: f.base_url, credential_set: Boolean(enc), enabled: f.enabled !== false, max_concurrent: f.max_concurrent ?? 0 });
    this.#rebuild();
    await this.discover(id);
    return this.get(id);
  }

  async update(a: User, id: number, body: Record<string, unknown>) {
    const before = this.#row(id);
    const f = this.#fields(body, true);
    if (f.name && f.name !== before.name && this.#db.prepare('SELECT 1 FROM llm_connections WHERE name = ?').get(f.name)) {
      throw new ApiError(409, 'CONFLICT', `A connection named ${f.name} exists.`);
    }
    const sets: string[] = [];
    const vals: (string | number | null)[] = [];
    const set = (col: string, v: string | number | null) => (sets.push(`${col} = ?`), vals.push(v));
    if (f.name !== undefined) set('name', f.name);
    if (f.base_url !== undefined) set('base_url', f.base_url);
    if (f.credential !== undefined) {
      set('credential_enc', f.credential ? encrypt(this.#needKey(), f.credential) : null);
      set('credential_hint', f.credential ? hint(f.credential) : null);
    }
    if (f.enabled !== undefined) set('enabled', f.enabled ? 1 : 0);
    if (f.max_concurrent !== undefined) set('max_concurrent', f.max_concurrent);
    if (body.position !== undefined) {
      if (!Number.isInteger(body.position) || (body.position as number) < 0) throw new ApiError(400, 'INVALID_REQUEST', 'position must be a non-negative integer.');
      set('position', body.position as number);
    }
    if (sets.length) this.#db.prepare(`UPDATE llm_connections SET ${sets.join(', ')} WHERE id = ?`).run(...vals, id);
    const changed = Object.fromEntries(Object.entries(f).filter(([k]) => k !== 'credential'));
    this.#record(a, 'llm.connection.update', {
      id, name: before.name, changed: { ...changed, ...(body.position !== undefined ? { position: body.position } : {}) }, credential_changed: f.credential !== undefined,
    });
    this.#rebuild();
    const after = this.#row(id);
    if (after.enabled && (f.base_url !== undefined || f.credential !== undefined || (f.enabled && !before.enabled))) await this.discover(id);
    return this.get(id);
  }

  async test(a: User, id: number) {
    const r = this.#row(id);
    await this.discover(id);
    this.#record(a, 'llm.connection.test', { id, name: r.name, status: this.#row(id).status });
    return this.get(id);
  }

  /** Where the connection's models are mapped in a policy in force. */
  usesOf(id: number): ModelUse[] {
    const r = this.#row(id);
    const models = new Set((this.#db.prepare('SELECT model_id FROM llm_models WHERE connection_id = ?').all(id) as { model_id: string }[]).map((m) => m.model_id));
    return this.#uses().filter((u) => {
      const { connection, model } = this.#split(u.model);
      // A bare id may reach every connection that offers it (first in order, the others on failover).
      return connection !== null ? connection === r.name : models.has(model);
    });
  }

  remove(a: User, id: number, confirm: boolean) {
    const r = this.#row(id);
    const uses = this.usesOf(id);
    if (uses.length && !confirm) {
      throw new ApiError(409, 'IN_USE', `The connection ${r.name} is used by ${uses.map((u) => `${u.scope} ${u.role}`).join(', ')}.`, { uses });
    }
    this.#db.prepare('DELETE FROM llm_connections WHERE id = ?').run(id);
    this.#record(a, 'llm.connection.delete', { id, name: r.name, in_use: uses });
    this.#rebuild();
    return { deleted: id };
  }

  setModel(a: User, id: number, model: string, body: Record<string, unknown>) {
    const r = this.#row(id);
    const before = this.#db.prepare('SELECT * FROM llm_models WHERE connection_id = ? AND model_id = ?').get(id, model) as ModelRow | undefined;
    if (!before) throw new ApiError(404, 'NOT_FOUND', 'No such model on this connection.');
    const kind = body.kind ?? before.kind;
    if (!(KINDS as readonly unknown[]).includes(kind)) throw new ApiError(400, 'INVALID_REQUEST', `kind must be one of ${KINDS.join(', ')}.`);
    const int = (k: string, min: number, max: number, cur: number | null, nullable: boolean) => {
      const v = body[k];
      if (v === undefined) return cur;
      if (v === null && nullable) return null;
      if (!Number.isInteger(v) || (v as number) < min || (v as number) > max) throw new ApiError(400, 'INVALID_REQUEST', `${k} must be an integer from ${min} to ${max}.`);
      return v as number;
    };
    const bool = (k: string, cur: number) => {
      const v = body[k];
      if (v === undefined) return cur;
      if (typeof v !== 'boolean') throw new ApiError(400, 'INVALID_REQUEST', `${k} must be true or false.`);
      return v ? 1 : 0;
    };
    const next = {
      kind: kind as string,
      context_window: int('context_window', 0, 10_000_000, before.context_window, false)!,
      max_output_tokens: int('max_output_tokens', 1, 1_000_000, before.max_output_tokens, true),
      tools: bool('tools', before.tools),
      images: bool('images', before.images),
      reasoning: bool('reasoning', before.reasoning),
      dimension: int('dimension', 1, 100_000, before.dimension, true),
    };
    this.#db
      .prepare('UPDATE llm_models SET kind = ?, context_window = ?, max_output_tokens = ?, tools = ?, images = ?, reasoning = ?, dimension = ? WHERE connection_id = ? AND model_id = ?')
      .run(next.kind, next.context_window, next.max_output_tokens, next.tools, next.images, next.reasoning, next.dimension, id, model);
    this.#record(a, 'llm.model.update', { connection: r.name, model, before: capabilityOf(before), after: capabilityOf({ ...before, ...next }) });
    return this.get(id);
  }

  /** Every known model with its connections, for the Models view pickers. */
  catalog() {
    const rows = this.#db
      .prepare(
        `SELECT c.name AS connection, c.enabled, m.model_id, m.kind, m.missing FROM llm_models m JOIN llm_connections c ON c.id = m.connection_id
         ORDER BY c.position, c.id, m.model_id`,
      )
      .all() as { connection: string; enabled: number; model_id: string; kind: string; missing: number }[];
    return { models: rows.map((m) => ({ connection: m.connection, id: m.model_id, kind: m.kind, enabled: Boolean(m.enabled), missing: Boolean(m.missing) })) };
  }
}
