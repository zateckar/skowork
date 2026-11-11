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
/** Reasoning efforts a chat model may be set to; which of them the upstream accepts is checked on save (D1). */
export const EFFORTS = ['none', 'minimal', 'low', 'medium', 'high', 'xhigh', 'max'] as const;
/** The fallback wait of a model without a stored one (add-model-fallback-and-effort D2). */
export const FALLBACK_AFTER_DEFAULT_S = 60;
/** The one-token probe of an effort on save and on Test (D1). */
const EFFORT_PROBE_TIMEOUT_MS = 20_000;

/** The model settings that the gateway applies to a chat request (D1, D2). */
export interface ModelSettings {
  reasoning_effort: string | null;
  fallback_model: string | null;
  fallback_after_s: number | null;
}

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
  reasoning_effort: string | null;
  fallback_model: string | null;
  fallback_after_s: number | null;
}

/** A stored model row with the name and state of its connection. */
type ResolvedRow = ModelRow & { connection_name: string; enabled: number };

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

/** Encrypts with the server key (add-llm-connections D1); the API connector secrets use the same format. */
export function encrypt(key: Buffer, plain: string): string {
  const iv = randomBytes(12);
  const c = createCipheriv('aes-256-gcm', key, iv);
  const ct = Buffer.concat([c.update(plain, 'utf8'), c.final()]);
  return 'v1.' + Buffer.concat([iv, c.getAuthTag(), ct]).toString('base64');
}

export function decrypt(key: Buffer, enc: string): string {
  const raw = Buffer.from(enc.replace(/^v1\./, ''), 'base64');
  const d = createDecipheriv('aes-256-gcm', key, raw.subarray(0, 12));
  d.setAuthTag(raw.subarray(12, 28));
  return Buffer.concat([d.update(raw.subarray(28)), d.final()]).toString('utf8');
}

const hint = (credential: string) => (credential.length > 8 ? credential.slice(-4) : '');

/**
 * Upstream text shown to an admin (add-model-fallback-and-effort D1): the credential, addresses and host names are
 * removed, and the text is kept short.
 */
export function sanitizeUpstreamText(text: string, secrets: string[]): string {
  let s = text;
  for (const k of secrets) if (k.length >= 4) s = s.split(k).join('[credential]');
  s = s
    .replace(/https?:\/\/\S+/gi, '[address]')
    .replace(/\b(?:\d{1,3}\.){3}\d{1,3}(?::\d+)?\b/g, '[address]')
    .replace(/\b[a-z][a-z0-9-]*(?:\.[a-z0-9-]+)+(?::\d+)?(?:\/\S*)?/gi, '[address]')
    .replace(/\b(?:sk|pk)-[A-Za-z0-9_-]{8,}/g, '[credential]')
    .replace(/\b(?:key|token)[-_][A-Za-z0-9_-]{8,}/gi, '[credential]')
    .replace(/\b[A-Za-z0-9_-]{32,}\b/g, '[credential]')
    .replace(/\s+/g, ' ')
    .trim();
  return s.length > 300 ? `${s.slice(0, 300)}…` : s;
}

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

const settingsOf = (m: ModelRow): ModelSettings => ({
  reasoning_effort: m.reasoning_effort,
  fallback_model: m.fallback_model,
  fallback_after_s: m.fallback_after_s,
});

/** reasoning_effort: undefined keeps the stored value; null or empty clears it (the model's own default). */
function effortField(v: unknown, cur: string | null): string | null {
  if (v === undefined) return cur;
  if (v === null || v === '') return null;
  if (typeof v !== 'string' || !(EFFORTS as readonly string[]).includes(v)) {
    throw new ApiError(400, 'INVALID_REQUEST', `reasoning_effort must be one of ${EFFORTS.join(', ')}, or null for the model's own default.`, { field: 'reasoning_effort' });
  }
  return v;
}

/** fallback_model: a model reference; undefined keeps the stored value; null or empty clears it. */
function fallbackField(v: unknown, cur: string | null): string | null {
  if (v === undefined) return cur;
  if (v === null || v === '') return null;
  if (typeof v !== 'string' || v.trim() === '' || v.length > 200) {
    throw new ApiError(400, 'INVALID_REQUEST', 'fallback_model must be a model id or connection/id.', { field: 'fallback_model' });
  }
  return v.trim();
}

/** fallback_after_s: 5 to 600 seconds; undefined keeps the stored value; null means the default (60 s). */
function afterField(v: unknown, cur: number | null): number | null {
  if (v === undefined) return cur;
  if (v === null) return null;
  if (!Number.isInteger(v) || (v as number) < 5 || (v as number) > 600) {
    throw new ApiError(400, 'INVALID_REQUEST', 'fallback_after_s must be an integer from 5 to 600, or null for the default of 60.', { field: 'fallback_after_s' });
  }
  return v as number;
}

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
  /** The server key for credentials, or null when WORKBENCH_SECRET_KEY is not set (API connector secrets use it too). */
  get secretKey(): Buffer | null {
    return this.#key;
  }
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

  /** The stored row a reference names: the first enabled one in connection order (the same choice as routing). */
  #resolve(ref: string): ResolvedRow | undefined {
    const { connection, model } = this.#split(ref);
    return this.#db
      .prepare(
        `SELECT m.*, c.name AS connection_name, c.enabled AS enabled FROM llm_models m JOIN llm_connections c ON c.id = m.connection_id
         WHERE m.model_id = ? AND (? IS NULL OR c.name = ?) ORDER BY c.enabled DESC, c.position, c.id LIMIT 1`,
      )
      .get(model, connection, connection) as ResolvedRow | undefined;
  }

  /**
   * A chat model the gateway accepts as a fallback (add-model-fallback-and-effort D2): listed by an enabled connection,
   * not missing, of kind chat.
   */
  #usableChat(ref: string): ResolvedRow | undefined {
    const { connection, model } = this.#split(ref);
    return this.#db
      .prepare(
        `SELECT m.*, c.name AS connection_name, c.enabled AS enabled FROM llm_models m JOIN llm_connections c ON c.id = m.connection_id
         WHERE m.model_id = ? AND (? IS NULL OR c.name = ?) AND c.enabled = 1 AND m.missing = 0 AND m.kind = 'chat'
         ORDER BY c.position, c.id LIMIT 1`,
      )
      .get(model, connection, connection) as ResolvedRow | undefined;
  }

  /** The stored capabilities of a mapped model (Model Capabilities From Connections). */
  capability(ref: string): ModelCapability {
    const row = this.#resolve(ref);
    return row ? capabilityOf(row) : guessCapability(this.#cfg, this.#split(ref).model);
  }

  /** The stored effort, fallback model and fallback wait of a mapped model; empty when none is stored. */
  settings(ref: string): ModelSettings {
    const row = this.#resolve(ref);
    return row ? settingsOf(row) : { reasoning_effort: null, fallback_model: null, fallback_after_s: null };
  }

  /** The labels (`connection/id`) of the models whose fallback is each model, keyed by connection and model id. */
  #fallbackLabels(): Map<string, string[]> {
    const out = new Map<string, string[]>();
    const rows = this.#db
      .prepare(
        `SELECT m.fallback_model, m.model_id, c.name AS connection_name FROM llm_models m JOIN llm_connections c ON c.id = m.connection_id
         WHERE m.fallback_model IS NOT NULL`,
      )
      .all() as { fallback_model: string; model_id: string; connection_name: string }[];
    for (const r of rows) {
      const target = this.#resolve(r.fallback_model);
      if (!target) continue;
      const key = `${target.connection_id}\n${target.model_id}`;
      out.set(key, [...(out.get(key) ?? []), `${r.connection_name}/${r.model_id}`]);
    }
    return out;
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

  #view(r: ConnectionRow, labels = this.#fallbackLabels()) {
    const models = (this.#db.prepare('SELECT * FROM llm_models WHERE connection_id = ? ORDER BY model_id').all(r.id) as unknown as ModelRow[]).map((m) => ({
      id: m.model_id,
      ...capabilityOf(m),
      last_seen: m.last_seen,
      missing: Boolean(m.missing),
      ...settingsOf(m),
      /** The models whose fallback this model is (`connection/id`); a model with any cannot have a fallback itself. */
      fallback_of: labels.get(`${m.connection_id}\n${m.model_id}`) ?? [],
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
    const labels = this.#fallbackLabels();
    return { connections: this.#rows().map((r) => this.#view(r, labels)), key_available: this.#key !== null };
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

  /**
   * Discovers the models, then checks the stored reasoning effort of each chat model (D1, Reasoning Effort In Connection
   * Test). The result is the connection's view plus effort_failures: the models whose upstream rejects their effort now.
   */
  async test(a: User, id: number) {
    const r = this.#row(id);
    await this.discover(id);
    const current = this.#row(id);
    const effort_failures = current.status === 'ok' ? await this.#checkStoredEfforts(current) : [];
    this.#record(a, 'llm.connection.test', { id, name: r.name, status: current.status, effort_failures: effort_failures.map((f) => f.model) });
    return { ...this.get(id), effort_failures };
  }

  async #checkStoredEfforts(c: ConnectionRow): Promise<{ model: string; reasoning_effort: string; reason: string }[]> {
    const rows = this.#db
      .prepare("SELECT * FROM llm_models WHERE connection_id = ? AND kind = 'chat' AND reasoning_effort IS NOT NULL AND missing = 0 ORDER BY model_id")
      .all(c.id) as unknown as ModelRow[];
    const key = this.#keyOf(c);
    const failures: { model: string; reasoning_effort: string; reason: string }[] = [];
    for (const m of rows) {
      const value = m.reasoning_effort!;
      if (key === null) {
        failures.push({ model: m.model_id, reasoning_effort: value, reason: 'the credential cannot be decrypted' });
        continue;
      }
      const res = await this.#probe(c.base_url, key, m.model_id, value);
      if (!res.ok) failures.push({ model: m.model_id, reasoning_effort: value, reason: res.reason });
    }
    return failures;
  }

  /** The stored credential of a connection: '' when none is set, null when it cannot be decrypted. */
  #keyOf(c: ConnectionRow): string | null {
    if (!c.credential_enc) return '';
    try {
      if (!this.#key) return null;
      return decrypt(this.#key, c.credential_enc);
    } catch {
      return null;
    }
  }

  /**
   * One one-token chat request with the effort to the upstream (D1). A 400, 404 or 422 is the upstream's verdict on
   * the value and carries its explanation, with the credential and any address removed. Other answers and network
   * failures say nothing about the value.
   */
  async #probe(baseUrl: string, key: string, model: string, value: string): Promise<{ ok: true } | { ok: false; rejected: boolean; reason: string }> {
    let res: Response;
    try {
      res = await fetch(`${baseUrl.replace(/\/+$/, '')}/chat/completions`, {
        method: 'POST',
        headers: { 'content-type': 'application/json', ...(key ? { authorization: `Bearer ${key}` } : {}), accept: 'application/json' },
        body: JSON.stringify({ model, messages: [{ role: 'user', content: 'Reply with OK.' }], max_tokens: 1, stream: false, reasoning_effort: value }),
        signal: AbortSignal.timeout(EFFORT_PROBE_TIMEOUT_MS),
      });
    } catch (err) {
      const reason = (err as Error).name === 'TimeoutError' ? 'the model server did not answer within 20 s' : 'the model server could not be reached';
      return { ok: false, rejected: false, reason };
    }
    if (res.ok) {
      await res.body?.cancel();
      return { ok: true };
    }
    const text = (await res.text().catch(() => '')).slice(0, 8192);
    if (![400, 404, 422].includes(res.status)) return { ok: false, rejected: false, reason: `the model server answered HTTP ${res.status}` };
    let message = '';
    try {
      const j = JSON.parse(text) as { error?: { message?: unknown } | unknown; message?: unknown };
      const e = typeof j.error === 'object' && j.error !== null ? (j.error as { message?: unknown }).message : j.error;
      message = typeof e === 'string' ? e : typeof j.message === 'string' ? j.message : '';
    } catch {
      message = text;
    }
    return { ok: false, rejected: true, reason: sanitizeUpstreamText(message || `HTTP ${res.status}`, key ? [key] : []) };
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

  /**
   * Saves a model's capabilities and settings (add-llm-connections D1, add-model-fallback-and-effort D1, D2). A reasoning
   * effort is checked against the upstream before it is stored; a refused value leaves the stored one as it was.
   */
  async setModel(a: User, id: number, model: string, body: Record<string, unknown>) {
    const r = this.#row(id);
    const before = this.#db.prepare('SELECT * FROM llm_models WHERE connection_id = ? AND model_id = ?').get(id, model) as ModelRow | undefined;
    if (!before) throw new ApiError(404, 'NOT_FOUND', 'No such model on this connection.');
    const kind = body.kind ?? before.kind;
    if (!(KINDS as readonly unknown[]).includes(kind)) throw new ApiError(400, 'INVALID_REQUEST', `kind must be one of ${KINDS.join(', ')}.`);
    const effort = effortField(body.reasoning_effort, before.reasoning_effort);
    const fallback = fallbackField(body.fallback_model, before.fallback_model);
    const afterS = afterField(body.fallback_after_s, before.fallback_after_s);
    if (kind !== 'chat' && (effort !== null || fallback !== null)) {
      throw new ApiError(400, 'INVALID_REQUEST', 'reasoning_effort and fallback_model apply to chat models only.', { field: effort !== null ? 'reasoning_effort' : 'fallback_model' });
    }
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
    // A fallback is checked when it changes, so saving other fields does not depend on the current state of the fallback.
    if (fallback !== null && fallback !== before.fallback_model) this.#checkFallback(before, fallback);
    // The effort is checked on every save that submits one, so an admin can re-check it by saving (design Risks).
    if (effort !== null && typeof body.reasoning_effort === 'string') await this.#checkEffort(a, before, effort);
    const settings = { reasoning_effort: effort, fallback_model: fallback, fallback_after_s: afterS };
    this.#db
      .prepare(
        `UPDATE llm_models SET kind = ?, context_window = ?, max_output_tokens = ?, tools = ?, images = ?, reasoning = ?, dimension = ?,
           reasoning_effort = ?, fallback_model = ?, fallback_after_s = ? WHERE connection_id = ? AND model_id = ?`,
      )
      .run(next.kind, next.context_window, next.max_output_tokens, next.tools, next.images, next.reasoning, next.dimension,
        settings.reasoning_effort, settings.fallback_model, settings.fallback_after_s, id, model);
    this.#record(a, 'llm.model.update', {
      connection: r.name,
      model,
      before: { ...capabilityOf(before), ...settingsOf(before) },
      after: { ...capabilityOf({ ...before, ...next }), ...settings },
    });
    return this.get(id);
  }

  #refuseEffort(a: User, row: ModelRow, value: string, reason: string, rejected: boolean): never {
    this.#audit.write({
      category: 'admin', action: 'llm.model.effort_refused', outcome: 'refused', subject: a.subject, userName: a.userName,
      details: { connection_id: row.connection_id, model: row.model_id, reasoning_effort: value, reason, rejected },
    });
    const message = rejected
      ? `${row.model_id} does not accept reasoning effort "${value}". The model server said: ${reason}`
      : `The reasoning effort "${value}" could not be checked: ${reason}.`;
    throw new ApiError(400, 'INVALID_REQUEST', message, { field: 'reasoning_effort', reason: rejected ? 'rejected' : 'unchecked' });
  }

  /** The one-token check of an effort, on an enabled connection that offers the model (D1). */
  async #checkEffort(a: User, row: ModelRow, value: string): Promise<void> {
    const target = this.#probeTarget(row);
    if (!target) this.#refuseEffort(a, row, value, `no enabled connection offers ${row.model_id}`, false);
    const key = this.#keyOf(target);
    if (key === null) this.#refuseEffort(a, row, value, 'the credential cannot be decrypted', false);
    const res = await this.#probe(target.base_url, key, row.model_id, value);
    if (!res.ok) this.#refuseEffort(a, row, value, res.reason, res.rejected);
  }

  /** The enabled connection the effort is checked on: the model's own connection when enabled, otherwise any that offers it. */
  #probeTarget(row: ModelRow): ConnectionRow | undefined {
    return this.#db
      .prepare(
        `SELECT c.* FROM llm_connections c JOIN llm_models m ON m.connection_id = c.id
         WHERE m.model_id = ? AND c.enabled = 1 ORDER BY (c.id = ?) DESC, c.position, c.id LIMIT 1`,
      )
      .get(row.model_id, row.connection_id) as ConnectionRow | undefined;
  }

  /** A fallback must be an enabled, listed chat model; not the model itself; with no fallback of its own; and the model must not be one (D2). */
  #checkFallback(row: ModelRow, ref: string): void {
    const field = { field: 'fallback_model' };
    const target = this.#usableChat(ref);
    if (!target) throw new ApiError(400, 'INVALID_REQUEST', `${ref} is not an enabled chat model that a connection lists.`, field);
    if (target.connection_id === row.connection_id && target.model_id === row.model_id) {
      throw new ApiError(400, 'INVALID_REQUEST', 'A model cannot be its own fallback.', field);
    }
    if (target.fallback_model !== null) {
      throw new ApiError(400, 'INVALID_REQUEST', `${ref} has a fallback of its own. A fallback chain is one step, so pick another model.`, field);
    }
    const of = this.#fallbackLabels().get(`${row.connection_id}\n${row.model_id}`);
    if (of?.length) {
      throw new ApiError(400, 'INVALID_REQUEST', `${row.model_id} is the fallback of ${of.join(', ')}, and a fallback cannot have a fallback of its own.`, field);
    }
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
