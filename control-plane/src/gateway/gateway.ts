// control-plane/llm-gateway (add-agent-runtime): role -> upstream model, rate limits, budgets, size checks,
// forwarding with the gateway's own credential, stream pass-through, failover before the first byte, usage
// accounting and the usage audit. Nothing about the upstream (address, key, error text) reaches the client.
import { Router, type Request, type Response } from 'express';
import type { Config, ModelCapability } from '../config.ts';
import { type Backend, Connections, FALLBACK_AFTER_DEFAULT_S } from './connections.ts';
import type { Audit } from '../identity/audit.ts';
import type { Db } from '../db.ts';
import { nowIso } from '../db.ts';
import { sendError, type ErrorExtra } from '../http.ts';
import type { User } from '../identity/identity.ts';

export const ROLES = ['main', 'vision', 'small', 'embed', 'rerank', 'image'] as const;
const isChat = (role: string) => role === 'main' || role === 'vision' || role === 'small';
// Image requests have their own budget, counted in images (add-image-generation design D2).
const bucketOf = (role: string) => (isChat(role) ? 'chat' : role === 'image' ? 'image' : 'embed');
type Kind = 'chat' | 'embeddings' | 'rerank' | 'images';
const IMAGE_SIZES = ['1024x1024', '1024x1536', '1536x1024'];
const MAX_IMAGES_PER_REQUEST = 4;
const MAX_IMAGE_PROMPT = 4000;

class GatewayError extends Error {
  readonly status: number;
  readonly code: string;
  readonly extra: ErrorExtra;
  constructor(status: number, code: string, message: string, extra: ErrorExtra = {}) {
    super(message);
    this.status = status;
    this.code = code;
    this.extra = extra;
  }
}
const unavailable = () => new GatewayError(503, 'MODEL_UNAVAILABLE', 'The model server is not available.', { reason: 'backend unavailable' });
const timeoutErr = () => new GatewayError(504, 'TIMEOUT', 'The model server did not answer in time.', { reason: 'backend timeout' });

interface Usage {
  prompt: number;
  completion: number;
  reasoning: number;
  total: number;
  /** Prompt tokens the upstream served from its cache (usage.prompt_tokens_details.cached_tokens); null when not reported. */
  cached: number | null;
}
const zero = (): Usage => ({ prompt: 0, completion: 0, reasoning: 0, total: 0, cached: null });
function usageFrom(u: unknown): Usage {
  const o = (u ?? {}) as Record<string, unknown>;
  const n = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : 0);
  const details = (o.completion_tokens_details ?? {}) as Record<string, unknown>;
  const promptDetails = (o.prompt_tokens_details ?? {}) as Record<string, unknown>;
  const prompt = n(o.prompt_tokens) || n(o.input_tokens);
  const completion = n(o.completion_tokens) || n(o.output_tokens);
  const cached = typeof promptDetails.cached_tokens === 'number' && Number.isFinite(promptDetails.cached_tokens) ? promptDetails.cached_tokens : null;
  return { prompt, completion, reasoning: n(details.reasoning_tokens), total: n(o.total_tokens) || prompt + completion, cached };
}

/** The outcome of one attempt on one backend: done (answered, or failed for good) or a failure that may fail over. */
type AttemptResult =
  | { done: true; usage: Usage; outcome: string; content?: unknown; error?: GatewayError; images?: number; ttft?: number }
  | { done: false; error: GatewayError };
type Done = Extract<AttemptResult, { done: true }>;

/** A model route: the upstream model id and the enabled connections that offer it, in order. */
type Route = { model: string; backends: Backend[] };

/** One leg of a request: a route with its body and limits (add-model-fallback-and-effort D3). */
interface Call {
  path: string;
  payload: string;
  kind: Kind;
  estimate: number;
  capture: boolean;
  out: EventStream | null;
  res: Response;
  /** Streamed: from the upstream request to the first byte. Non-streamed: to the response headers. */
  firstByteMs: number;
  /** Non-streamed: the limit of the whole attempt, headers and body. */
  totalMs: number;
  /** The client's abort. */
  signal: AbortSignal;
  /** A fallback leg: when no backend is healthy, it may still use one that is marked unhealthy (D3). */
  rescue: boolean;
}

/** A leg's outcome: answered (or failed for good), or every attempt on its route failed before the first byte. */
type LegResult = { done: true; result: Done } | { done: false; error: GatewayError };

/** Waits for p, or rejects with the signal's reason when the signal aborts first (a body read can outlive its abort). */
function raceAbort<T>(p: Promise<T>, signal: AbortSignal): Promise<T> {
  if (signal.aborted) return Promise.reject(signal.reason);
  let onAbort: () => void = () => {};
  const aborted = new Promise<never>((_, reject) => {
    onAbort = () => reject(signal.reason);
    signal.addEventListener('abort', onAbort, { once: true });
  });
  return Promise.race([p, aborted]).finally(() => signal.removeEventListener('abort', onAbort));
}

/** Thrown into a streamed attempt when one of its own limits fires (fix-long-thread-model-calls D2). */
class StreamLimit extends Error {
  readonly limit: 'first-byte' | 'idle' | 'max';
  constructor(limit: 'first-byte' | 'idle' | 'max') {
    super(`stream ${limit} limit`);
    this.limit = limit;
  }
}

/**
 * The SSE response of a streamed chat request (fix-long-thread-model-calls D1). The headers are sent when a backend has
 * been acquired; a keep-alive comment follows whenever nothing was written for keepAliveMs. Comments are not upstream
 * bytes, so they do not count against failover.
 */
class EventStream {
  readonly #res: Response;
  readonly #keepAliveMs: number;
  #started = false;
  #timer: NodeJS.Timeout | undefined;
  constructor(res: Response, keepAliveMs: number) {
    this.#res = res;
    this.#keepAliveMs = keepAliveMs;
  }
  get started() {
    return this.#started;
  }
  get closed() {
    return this.#res.writableEnded || this.#res.destroyed;
  }
  start() {
    this.#started = true;
    this.#res.status(200);
    this.#res.setHeader('Content-Type', 'text/event-stream; charset=utf-8');
    this.#res.setHeader('Cache-Control', 'no-cache');
    this.#res.setHeader('X-Accel-Buffering', 'no');
    this.#res.flushHeaders();
    this.#arm();
  }
  write(data: Buffer | string) {
    if (this.closed) return;
    this.#res.write(data);
    this.#arm();
  }
  /** One error event, in the shape of the error body (sendError), then the end of the stream without [DONE]. */
  error(code: string, message: string, extra: ErrorExtra) {
    this.write(`data: ${JSON.stringify({ error: { code, message, ...extra } })}\n\n`);
    this.end();
  }
  end() {
    clearTimeout(this.#timer);
    this.#timer = undefined;
    if (!this.closed) this.#res.end();
  }
  #arm() {
    clearTimeout(this.#timer);
    this.#timer = setTimeout(() => {
      if (this.closed) return;
      this.#res.write(': keep-alive\n\n');
      this.#arm();
    }, this.#keepAliveMs);
  }
}

// ---- backends -------------------------------------------------------------------------------------

/** Health and capacity over the enabled connections (add-llm-connections D3: each connection is a backend). */
export class BackendPool {
  readonly #connections: Connections;
  readonly #timer: NodeJS.Timeout;
  readonly #log: (m: string) => void;
  constructor(cfg: Config, connections: Connections, log: (m: string) => void) {
    this.#connections = connections;
    this.#log = log;
    this.#timer = setInterval(() => void this.#probeUnhealthy(), cfg.gateway.healthCheckIntervalMs);
    this.#timer.unref();
  }
  get backends(): Backend[] {
    return this.#connections.backends;
  }
  close() {
    clearInterval(this.#timer);
  }
  /**
   * The first healthy candidate with capacity that was not tried yet, in connection order; allBusy tells whether
   * capacity was the only obstacle.
   */
  acquire(tried: Set<Backend>, candidates: Backend[]): { backend: Backend | null; allBusy: boolean } {
    const healthy = candidates.filter((b) => b.healthy && !tried.has(b));
    const b = healthy.find((x) => x.maxConcurrent === 0 || x.inFlight < x.maxConcurrent) ?? null;
    if (b) b.inFlight++;
    return { backend: b, allBusy: !b && healthy.length > 0 };
  }
  /**
   * A backend for a fallback when none is healthy (add-model-fallback-and-effort D3): a connection that the model's own
   * failure marked unhealthy may still answer the fallback, since its other models share it.
   */
  acquireUnhealthy(tried: Set<Backend>, candidates: Backend[]): Backend | null {
    const b = candidates.find((x) => !tried.has(x) && (x.maxConcurrent === 0 || x.inFlight < x.maxConcurrent)) ?? null;
    if (b) b.inFlight++;
    return b;
  }
  release(b: Backend) {
    b.inFlight = Math.max(0, b.inFlight - 1);
  }
  markUnhealthy(b: Backend) {
    if (b.healthy) this.#log(`${b} marked unhealthy`);
    b.healthy = false;
  }
  async #probeUnhealthy() {
    for (const b of this.backends.filter((x) => !x.healthy)) {
      try {
        const r = await fetch(b.endpoint('models'), { headers: { authorization: `Bearer ${b.apiKey}` }, signal: AbortSignal.timeout(5000) });
        if (r.ok) {
          b.healthy = true;
          this.#log(`${b} healthy again`);
        }
        await r.body?.cancel();
      } catch {
        /* still down */
      }
    }
  }
}

// ---- rate limits and budgets ---------------------------------------------------------------------

class RateLimiter {
  readonly #windows = new Map<string, number[]>();
  readonly #active = new Map<string, number>();
  enter(subject: string, rpm: number, concurrent: number): { ok: true; release: () => void } | { ok: false; retryAfter: number } {
    const now = Date.now();
    const w = (this.#windows.get(subject) ?? []).filter((t) => now - t < 60_000);
    if ((this.#active.get(subject) ?? 0) >= concurrent) return { ok: false, retryAfter: 1 };
    if (w.length >= rpm) return { ok: false, retryAfter: Math.max(1, Math.ceil((60_000 - (now - w[0]!)) / 1000)) };
    w.push(now);
    this.#windows.set(subject, w);
    this.#active.set(subject, (this.#active.get(subject) ?? 0) + 1);
    let released = false;
    return {
      ok: true,
      release: () => {
        if (released) return;
        released = true;
        this.#active.set(subject, Math.max(0, (this.#active.get(subject) ?? 1) - 1));
      },
    };
  }
}

export function periodStart(period: 'day' | 'week' | 'month', now = new Date()): Date {
  const d = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate()));
  if (period === 'week') d.setUTCDate(d.getUTCDate() - ((d.getUTCDay() + 6) % 7)); // Monday
  if (period === 'month') d.setUTCDate(1);
  return d;
}
export function periodReset(period: 'day' | 'week' | 'month', now = new Date()): Date {
  const s = periodStart(period, now);
  if (period === 'day') s.setUTCDate(s.getUTCDate() + 1);
  else if (period === 'week') s.setUTCDate(s.getUTCDate() + 7);
  else s.setUTCMonth(s.getUTCMonth() + 1);
  return s;
}

// ---- estimates ------------------------------------------------------------------------------------

function estimateChatTokens(root: Record<string, unknown>, imageTokens: number): number {
  let chars = 0;
  for (const m of Array.isArray(root.messages) ? root.messages : []) {
    const c = (m as Record<string, unknown>)?.content;
    if (typeof c === 'string') chars += c.length;
    else if (Array.isArray(c)) {
      for (const p of c as Record<string, unknown>[]) {
        if (typeof p?.text === 'string') chars += p.text.length;
        else if (p?.type === 'image_url' || p?.type === 'input_image') chars += imageTokens * 4;
      }
    }
    const calls = (m as Record<string, unknown>)?.tool_calls;
    if (calls) chars += JSON.stringify(calls).length;
  }
  if (root.tools) chars += JSON.stringify(root.tools).length;
  return Math.ceil(chars / 4);
}
const estimateInputTokens = (root: Record<string, unknown>) =>
  Math.ceil(['input', 'query', 'documents'].reduce((n, k) => n + (root[k] === undefined ? 0 : JSON.stringify(root[k]).length), 0) / 4);

/** Whether a chat request has tools to call (a non-empty tools array). */
const hasTools = (root: Record<string, unknown>) => Array.isArray(root.tools) && root.tools.length > 0;
/** Whether a chat request carries an image part (add-model-fallback-and-effort D3 fit check). */
function hasImageParts(root: Record<string, unknown>): boolean {
  return (Array.isArray(root.messages) ? root.messages : []).some((m) => {
    const c = (m as Record<string, unknown>)?.content;
    return Array.isArray(c) && (c as Record<string, unknown>[]).some((p) => p?.type === 'image_url' || p?.type === 'input_image');
  });
}

/** The audit's fallback reason for the failure that made the gateway switch (D5). */
const fallbackReason = (e: GatewayError): string => (e.code === 'TIMEOUT' ? 'timeout' : e.code === 'RATE_LIMIT' ? 'capacity' : 'unavailable');

/** Scans an SSE stream for [DONE], finish reasons, usage and (at content level) the assembled content. */
class SseScanner {
  sawDone = false;
  sawFinish = false;
  usage: Usage | null = null;
  bytes = 0;
  content = '';
  reasoning = '';
  #buf = '';
  readonly #capture: boolean;
  readonly #max: number;
  constructor(capture: boolean, max: number) {
    this.#capture = capture;
    this.#max = max;
  }
  feed(chunk: Buffer) {
    this.bytes += chunk.length;
    this.#buf += chunk.toString('utf8');
    let i: number;
    while ((i = this.#buf.indexOf('\n')) >= 0) {
      const line = this.#buf.slice(0, i).replace(/\r$/, '');
      this.#buf = this.#buf.slice(i + 1);
      if (!line.startsWith('data:')) continue;
      const data = line.slice(5).trim();
      if (data === '[DONE]') {
        this.sawDone = true;
        continue;
      }
      try {
        const j = JSON.parse(data) as { usage?: unknown; choices?: { finish_reason?: unknown; delta?: { content?: unknown; reasoning_content?: unknown } }[] };
        if (j.usage) this.usage = usageFrom(j.usage);
        for (const c of j.choices ?? []) {
          if (c.finish_reason) this.sawFinish = true;
          if (this.#capture && this.content.length < this.#max && typeof c.delta?.content === 'string') this.content += c.delta.content;
          if (this.#capture && this.reasoning.length < this.#max && typeof c.delta?.reasoning_content === 'string') this.reasoning += c.delta.reasoning_content;
        }
      } catch {
        /* not JSON: pass through */
      }
    }
  }
}

// ---- the gateway ----------------------------------------------------------------------------------

export class Gateway {
  readonly #cfg: Config;
  readonly #db: Db;
  readonly connections: Connections;
  readonly pool: BackendPool;
  readonly #limiter = new RateLimiter();
  readonly #log: (m: string) => void;
  /** Model reference -> the time until which its requests go straight to its fallback (D4). In memory, per process. */
  readonly #cooldown = new Map<string, number>();

  constructor(cfg: Config, db: Db, audit: Audit, log: (m: string) => void) {
    this.#cfg = cfg;
    this.#db = db;
    this.#log = log;
    this.connections = new Connections(cfg, db, audit, log);
    this.pool = new BackendPool(cfg, this.connections, log);
  }

  close() {
    this.pool.close();
    this.connections.close();
  }

  /** The user's effective policy keys (control-plane/policy-service); unset keys take the server-side defaults. */
  #policy: (user: User) => ReadonlyMap<string, unknown> = () => new Map();

  setPolicySource(fn: (user: User) => ReadonlyMap<string, unknown>): void {
    this.#policy = fn;
  }

  /** The user's effective policy.llm.models; a role the mapping leaves out is not available (Role Mapping). */
  modelsFor(user: User): Record<string, string> {
    return (this.#policy(user).get('llm.models') as Record<string, string> | undefined) ?? this.#cfg.gateway.models;
  }

  /** Budgets and rate limits from policy.llm.budget and policy.llm.rate_limits, defaulting to the server settings. */
  #limitsFor(user: User) {
    const p = this.#policy(user);
    const g = this.#cfg.gateway;
    const n = (k: string, d: number) => (typeof p.get(k) === 'number' ? (p.get(k) as number) : d);
    return {
      budgetTokens: n('llm.budget.tokens', g.budgetTokens),
      budgetPeriod: (p.get('llm.budget.period') as 'day' | 'week' | 'month' | undefined) ?? g.budgetPeriod,
      embedDailyTokens: n('llm.budget.embed_daily_tokens', g.embedDailyTokens),
      imagesDaily: n('llm.budget.images_daily', g.imagesDaily),
      requestsPerMinute: n('llm.rate_limits.requests_per_minute', g.requestsPerMinute),
      concurrentRequests: n('llm.rate_limits.concurrent_requests', g.concurrentRequests),
    };
  }

  #capability(upstream: string): ModelCapability {
    return this.connections.capability(upstream);
  }

  /** Tokens used in a bucket this period; for the image bucket, generated images. */
  #used(subject: string, bucket: string, period: 'day' | 'week' | 'month'): number {
    const column = bucket === 'image' ? 'images' : 'total_tokens';
    const row = this.#db
      .prepare(`SELECT COALESCE(SUM(${column}), 0) AS t FROM llm_usage WHERE subject = ? AND bucket = ? AND at >= ?`)
      .get(subject, bucket, periodStart(period).toISOString()) as { t: number };
    return Number(row.t);
  }

  #record(user: User, req: Request, endpoint: string, role: string | null, upstream: string | null, usage: Usage, outcome: string, started: number, content: unknown,
    images = 0, ttft: number | null = null, answeredBy: string | null = null, fallback: string | null = null) {
    this.#insertUsage(user, req.header('x-workbench-installation')?.slice(0, 128) ?? null, req.header('x-workbench-run')?.slice(0, 128) ?? null,
      endpoint, role, upstream, usage, outcome, started, content, images, ttft, answeredBy, fallback);
  }

  #insertUsage(user: User, installation: string | null, run: string | null, endpoint: string, role: string | null, upstream: string | null, usage: Usage,
    outcome: string, started: number, content: unknown, images = 0, ttft: number | null = null, answeredBy: string | null = null, fallback: string | null = null) {
    this.#db
      .prepare(
        `INSERT INTO llm_usage (at, subject, user_name, installation_id, endpoint, role, upstream_model, bucket, prompt_tokens, completion_tokens,
           reasoning_tokens, total_tokens, outcome, duration_ms, content, run_id, groups, images, ttft_ms, cached_tokens, answered_by, fallback)
           VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
      )
      .run(nowIso(), user.subject, user.userName, installation, endpoint, role, upstream,
        role ? bucketOf(role) : null, usage.prompt, usage.completion, usage.reasoning, usage.total, outcome, Date.now() - started,
        content === undefined ? null : JSON.stringify(content), run, JSON.stringify(user.groups ?? []), images, ttft, usage.cached, answeredBy, fallback);
  }

  /**
   * Embeds texts with the user's `embed` role on their behalf (control-plane/team-knowledge), within their embedding
   * budget and recorded in the usage audit like a client request. Null when the role, the budget or every connection
   * is unavailable; the vectors come with the mapped model, so vectors of different models are never compared.
   */
  async embedFor(user: User, input: string[]): Promise<{ model: string; vectors: number[][] } | null> {
    const upstream = this.modelsFor(user).embed;
    if (!upstream || !input.length) return null;
    const lim = this.#limitsFor(user);
    if (this.#used(user.subject, 'embed', 'day') >= lim.embedDailyTokens) return null;
    const route = this.connections.route(upstream);
    const started = Date.now();
    const tried = new Set<Backend>();
    for (;;) {
      const { backend } = this.pool.acquire(tried, route.backends);
      if (!backend) {
        this.#insertUsage(user, null, null, 'embeddings', 'embed', upstream, zero(), 'rejected:model_unavailable', started, undefined);
        return null;
      }
      tried.add(backend);
      try {
        const r = await fetch(backend.endpoint('embeddings'), {
          method: 'POST',
          headers: { 'content-type': 'application/json', authorization: `Bearer ${backend.apiKey}`, accept: 'application/json' },
          body: JSON.stringify({ model: route.model, input }),
          signal: AbortSignal.timeout(this.#cfg.gateway.requestTimeoutMs),
        });
        if (!r.ok) {
          await r.body?.cancel();
          if (r.status === 401 || r.status === 403 || r.status >= 500) this.pool.markUnhealthy(backend);
          if (r.status === 429 || r.status === 401 || r.status === 403 || r.status >= 500) continue;
          this.#insertUsage(user, null, null, 'embeddings', 'embed', upstream, zero(), 'rejected:invalid_request', started, undefined);
          return null;
        }
        const j = (await r.json()) as { data?: { index?: number; embedding?: number[] }[]; usage?: unknown };
        const data = [...(j.data ?? [])].sort((a, b) => (a.index ?? 0) - (b.index ?? 0));
        if (data.length !== input.length || !data.every((d) => Array.isArray(d.embedding))) return null;
        let usage = j.usage ? usageFrom(j.usage) : zero();
        if (usage.total === 0) {
          const est = estimateInputTokens({ input });
          usage = { prompt: est, completion: 0, reasoning: 0, total: est, cached: null };
        }
        this.#insertUsage(user, null, null, 'embeddings', 'embed', upstream, usage, 'ok', started, undefined);
        return { model: upstream, vectors: data.map((d) => d.embedding!) };
      } catch {
        this.pool.markUnhealthy(backend);
      } finally {
        this.pool.release(backend);
      }
    }
  }

  listModels(req: Request, res: Response) {
    const started = Date.now();
    const data: Record<string, unknown>[] = [];
    const models = this.modelsFor(req.user!);
    for (const role of ROLES) {
      const upstream = models[role];
      if (!upstream) continue;
      const cap = this.#capability(upstream);
      const entry: Record<string, unknown> = { id: role, object: 'model', owned_by: 'workbench' };
      if (isChat(role)) {
        Object.assign(entry, { kind: 'chat', context_window: cap.context_window, max_output_tokens: cap.max_output_tokens ?? 4096,
          tools: Boolean(cap.tools), images: Boolean(cap.images), reasoning: Boolean(cap.reasoning) });
      } else if (role === 'embed') {
        Object.assign(entry, { kind: 'embed', dimension: cap.dimension ?? 0, model: this.connections.route(upstream).model });
      } else if (role === 'image') {
        entry.kind = 'image';
      } else {
        entry.kind = 'rerank';
      }
      data.push(entry);
    }
    this.#record(req.user!, req, 'models', null, null, zero(), 'ok', started, undefined);
    res.json({ object: 'list', data });
  }

  /** The fallback of a model, when it fits the request: its route and its effort (D3). Null when it cannot take the request. */
  #fallbackTo(ref: string, root: Record<string, unknown>, estimate: number): { ref: string; route: Route; effort: string | null } | null {
    const route = this.connections.route(ref);
    if (!route.backends.length) return null;
    const cap = this.#capability(ref);
    const fits = estimate <= cap.context_window && (!hasTools(root) || Boolean(cap.tools)) && (!hasImageParts(root) || Boolean(cap.images));
    return fits ? { ref, route, effort: this.connections.settings(ref).reasoning_effort } : null;
  }

  async handle(req: Request, res: Response, kind: Kind): Promise<void> {
    const user = req.user!;
    const started = Date.now();
    let role: string | null = null;
    let upstream: string | null = null;
    let usage = zero();
    let outcome = 'ok';
    let content: unknown;
    let images = 0;
    let ttft: number | null = null;
    let answeredBy: string | null = null;
    let fallback: string | null = null;
    let release: (() => void) | null = null;
    let out: EventStream | null = null; // set for a streamed chat request once its checks have passed
    const abort = new AbortController();
    res.on('close', () => {
      if (!res.writableFinished) abort.abort();
    });
    // Before the stream starts, a failure is an HTTP error. After it, the status is gone, so the failure is one error event.
    const fail = (e: GatewayError) => {
      outcome = 'rejected:' + e.code.toLowerCase();
      if (out?.started) out.error(e.code, e.message, e.extra);
      else if (!res.headersSent) sendError(res, e.status, e.code, e.message, e.extra);
    };
    try {
      const root = req.body as Record<string, unknown>;
      if (!root || typeof root !== 'object' || Array.isArray(root)) return fail(new GatewayError(400, 'INVALID_REQUEST', 'The request body must be a JSON object.'));
      role = typeof root.model === 'string' ? root.model.slice(0, 64) : null;
      if (!role) return fail(new GatewayError(400, 'INVALID_REQUEST', 'model is required.'));
      const models = this.modelsFor(user);
      upstream = (ROLES as readonly string[]).includes(role) ? (models[role] ?? null) : null;
      if (!upstream) return fail(new GatewayError(403, 'MODEL_UNAVAILABLE', 'The requested model role is not available to you.', { reason: 'role not granted' }));
      const fits = kind === 'chat' ? isChat(role) : kind === 'embeddings' ? role === 'embed' : kind === 'images' ? role === 'image' : role === 'rerank';
      if (!fits) return fail(new GatewayError(400, 'INVALID_REQUEST', 'This model role cannot be used with this endpoint.', { reason: 'role not valid for endpoint' }));

      const g = this.#cfg.gateway;
      const lim = this.#limitsFor(user);
      const lease = this.#limiter.enter(user.subject, lim.requestsPerMinute, lim.concurrentRequests);
      if (!lease.ok) return fail(new GatewayError(429, 'RATE_LIMIT', 'Too many requests.', { reason: 'rate', retry_after_s: lease.retryAfter }));
      release = lease.release;

      const bucket = bucketOf(role);
      let imageRequest: { prompt: string; n: number; size?: string } | null = null;
      if (kind === 'images') {
        const n = root.n === undefined ? 1 : root.n;
        if (typeof root.prompt !== 'string' || !root.prompt.trim() || root.prompt.length > MAX_IMAGE_PROMPT) {
          return fail(new GatewayError(400, 'INVALID_REQUEST', `prompt must be a non-empty string of at most ${MAX_IMAGE_PROMPT} characters.`));
        }
        if (!Number.isInteger(n) || (n as number) < 1 || (n as number) > MAX_IMAGES_PER_REQUEST) {
          return fail(new GatewayError(400, 'INVALID_REQUEST', `n must be an integer from 1 to ${MAX_IMAGES_PER_REQUEST}.`));
        }
        if (root.size !== undefined && !IMAGE_SIZES.includes(root.size as string)) {
          return fail(new GatewayError(400, 'INVALID_REQUEST', `size must be one of ${IMAGE_SIZES.join(', ')}.`));
        }
        imageRequest = { prompt: root.prompt, n: n as number, ...(root.size === undefined ? {} : { size: root.size as string }) };
        // Checked before forwarding: the whole request must fit in what is left of today's image budget.
        if (this.#used(user.subject, bucket, 'day') + imageRequest.n > lim.imagesDaily) {
          return fail(new GatewayError(429, 'QUOTA_EXCEEDED', 'The image budget is used up.', { reason: 'quota', reset_at: periodReset('day').toISOString() }));
        }
      } else {
        const period = bucket === 'chat' ? lim.budgetPeriod : 'day';
        const limit = bucket === 'chat' ? lim.budgetTokens : lim.embedDailyTokens;
        if (this.#used(user.subject, bucket, period) >= limit) {
          return fail(new GatewayError(429, 'QUOTA_EXCEEDED', 'The token budget is used up.', { reason: 'quota', reset_at: periodReset(period).toISOString() }));
        }
      }

      const estimate = kind === 'chat' ? estimateChatTokens(root, g.imageTokenEstimate) : kind === 'images' ? 0 : estimateInputTokens(root);
      if (kind === 'chat' && estimate > this.#capability(upstream).context_window) {
        return fail(new GatewayError(400, 'CONTEXT_LENGTH', "The request is larger than the model's context window.", { reason: 'context window' }));
      }

      // The mapped id resolves to the enabled connections that offer it (Role Mapping To Connection Models).
      const route = this.connections.route(upstream);
      // The model's settings (D1, D2) and its fallback, when the request fits the fallback (add-model-fallback-and-effort D3).
      const msettings = kind === 'chat' ? this.connections.settings(upstream) : null;
      const fb = msettings?.fallback_model ? this.#fallbackTo(msettings.fallback_model, root, estimate) : null;
      if (!route.backends.length && !fb) {
        return fail(new GatewayError(503, 'MODEL_UNAVAILABLE', 'The model server is not available.', { reason: 'no enabled connection offers the model' }));
      }
      const stream = kind === 'chat' && root.stream === true;
      if (stream) out = new EventStream(res, g.keepAliveMs);
      const capture = kind === 'chat' && g.telemetryLevel === 'content'; // never for embeddings or rerank
      const requestCopy = capture ? structuredClone(root) : undefined;
      if (imageRequest && g.telemetryLevel === 'content') content = { request: imageRequest };
      const path = kind === 'chat' ? 'chat/completions' : kind === 'images' ? 'images/generations' : kind;
      const total = kind === 'images' ? g.imageTimeoutMs : g.requestTimeoutMs;
      // A streamed request limits its first byte by the stream limit; a non-streamed one by its whole limit.
      const normalFirst = stream ? g.firstTokenTimeoutMs : total;
      // The body for a route: the routed model id, the effort (null removes it; undefined keeps the client's body) and,
      // for a streamed request, the usage option. An image request forwards only its prompt, count and size.
      const body = (model: string, effort: string | null | undefined) => {
        const forwarded: Record<string, unknown> = imageRequest ? { model, ...imageRequest } : { ...root, model };
        if (effort === null) delete forwarded.reasoning_effort;
        else if (effort !== undefined) forwarded.reasoning_effort = effort;
        if (stream) forwarded.stream_options = { ...((root.stream_options as object) ?? {}), include_usage: true };
        return JSON.stringify(forwarded);
      };
      const call = (model: string, effort: string | null | undefined, firstByteMs: number, rescue = false): Call => ({
        path, payload: body(model, effort), kind, estimate, capture, out, res, firstByteMs, totalMs: total, signal: abort.signal, rescue,
      });

      // The leg that answers: the model itself, or its fallback (D3) while it is cooling down (D4) or after it failed.
      let leg: LegResult;
      let servedBy = upstream;
      if (fb && (this.#cooldown.get(upstream) ?? 0) > Date.now()) {
        fallback = 'cooldown';
        servedBy = fb.ref;
        leg = await this.#leg(fb.route, call(fb.route.model, fb.effort ?? null, normalFirst, true));
      } else if (fb) {
        const afterMs = (msettings!.fallback_after_s ?? FALLBACK_AFTER_DEFAULT_S) * 1000;
        leg = await this.#leg(route, call(route.model, msettings!.reasoning_effort ?? undefined, afterMs));
        if (!leg.done) {
          fallback = fallbackReason(leg.error);
          servedBy = fb.ref;
          this.#cooldown.set(upstream, Date.now() + g.fallbackCooldownMs);
          this.#log(`${upstream} fell back to ${fb.ref} (${fallback})`);
          leg = await this.#leg(fb.route, call(fb.route.model, fb.effort ?? null, normalFirst, true));
        }
      } else {
        leg = await this.#leg(route, call(route.model, msettings?.reasoning_effort ?? undefined, normalFirst));
      }

      if (!leg.done) return fail(leg.error);
      usage = leg.result.usage;
      outcome = leg.result.outcome;
      images = leg.result.images ?? 0;
      ttft = leg.result.ttft ?? null;
      answeredBy = kind === 'chat' ? servedBy : null;
      if (capture) content = { request: requestCopy, response: leg.result.content };
      if (leg.result.error) fail(leg.result.error);
    } catch (err) {
      if (abort.signal.aborted) {
        outcome = 'client-aborted';
        return;
      }
      this.#log(`gateway failure: ${(err as Error).name}`);
      fail(new GatewayError(500, 'INTERNAL', 'The gateway could not process the request.'));
      outcome = 'error:internal';
    } finally {
      release?.();
      out?.end();
      this.#record(user, req, kind, role, upstream, usage, outcome, started, content, images, ttft, answeredBy, fallback);
    }
  }

  /**
   * Tries the backends of one route in order, each once, until one answers or fails for good. Returns the failure of
   * the last attempt when none is left (Backend Failover).
   */
  async #leg(route: Route, call: Call): Promise<LegResult> {
    const tried = new Set<Backend>();
    let lastError: GatewayError | null = null;
    for (;;) {
      const picked = this.pool.acquire(tried, route.backends);
      const allBusy = picked.allBusy;
      let backend = picked.backend;
      if (!backend && call.rescue) backend = this.pool.acquireUnhealthy(tried, route.backends);
      if (!backend) {
        return {
          done: false,
          error:
            lastError ??
            (allBusy
              ? new GatewayError(429, 'RATE_LIMIT', 'The model servers are at capacity.', { reason: 'rate', retry_after_s: 1 })
              : new GatewayError(503, 'MODEL_UNAVAILABLE', 'No model server is available right now.', { reason: 'no healthy backend' })),
        };
      }
      tried.add(backend);
      // The stream starts as soon as a backend is held; failover to another backend stays possible until the first byte.
      if (call.out && !call.out.started) call.out.start();
      try {
        const attempt = await this.#try(backend, call);
        if (attempt.done) return { done: true, result: attempt };
        lastError = attempt.error;
      } finally {
        this.pool.release(backend);
      }
    }
  }

  async #try(b: Backend, call: Call): Promise<AttemptResult> {
    if (call.out) return this.#stream(b, call);
    const { path, payload, kind, estimate, capture, res, signal: clientAbort } = call;
    const g = this.#cfg.gateway;
    // The first limit covers the wait for the response headers; the whole attempt is still bounded by the total limit.
    const ctl = new AbortController();
    const signal = AbortSignal.any([clientAbort, ctl.signal]);
    const limitFired = () => ctl.signal.aborted && ctl.signal.reason instanceof StreamLimit;
    const t0 = Date.now();
    let timer: NodeJS.Timeout | undefined = setTimeout(() => ctl.abort(new StreamLimit('first-byte')), call.firstByteMs);
    try {
      let upstreamRes: globalThis.Response;
      try {
        // Only the gateway's own credential is sent; nothing of the client's request headers is forwarded.
        upstreamRes = await fetch(b.endpoint(path), {
          method: 'POST',
          headers: { 'content-type': 'application/json', authorization: `Bearer ${b.apiKey}`, accept: 'application/json' },
          body: payload,
          signal,
        });
      } catch (err) {
        if (clientAbort.aborted) throw err;
        if (limitFired()) return { done: false, error: timeoutErr() };
        this.#log(`${b} unreachable: ${(err as Error).name}`);
        this.pool.markUnhealthy(b);
        return { done: false, error: unavailable() };
      }
      // Prompt Cache Audit (D7): for a non-streamed request, the time to the response headers stands for the first byte.
      const ttft = Date.now() - t0;
      clearTimeout(timer);
      timer = setTimeout(() => ctl.abort(new StreamLimit('max')), Math.max(1, call.totalMs - (Date.now() - t0)));

      if (!upstreamRes.ok) {
        const snippet = (await upstreamRes.text().catch(() => '')).slice(0, 8192);
        if (clientAbort.aborted) throw new Error('client aborted');
        if (limitFired()) return { done: false, error: timeoutErr() };
        const { error, retry } = this.#classify(b, upstreamRes, snippet);
        if (retry) return { done: false, error };
        return { done: true, usage: zero(), outcome: 'rejected:' + error.code.toLowerCase(), error };
      }

      let text: string;
      try {
        text = await upstreamRes.text();
      } catch {
        if (clientAbort.aborted) throw new Error('client aborted');
        if (limitFired()) return { done: false, error: timeoutErr() };
        return { done: false, error: unavailable() };
      }
      let json: Record<string, unknown> | null = null;
      try {
        json = JSON.parse(text);
      } catch {
        /* pass through */
      }
      let usage = json?.usage ? usageFrom(json.usage) : zero();
      if (kind === 'rerank' && usage.total === 0) {
        const billed = Number((json?.meta as { billed_units?: { input_tokens?: number } })?.billed_units?.input_tokens ?? 0);
        usage = { prompt: billed, completion: 0, reasoning: 0, total: billed, cached: null };
      }
      if (kind === 'images') {
        // The upstream reports zero tokens for images; the image budget counts the images returned.
        res.status(200).type('application/json').send(text);
        return { done: true, usage: zero(), outcome: 'ok', images: Array.isArray(json?.data) ? json.data.length : 0, ttft };
      }
      if (usage.total === 0) usage = { prompt: estimate, completion: 0, reasoning: 0, total: estimate, cached: null };
      if (kind === 'embeddings' && json && Array.isArray(json.data) && Array.isArray((json.data[0] as { embedding?: unknown[] })?.embedding)) {
        json.dimension = ((json.data[0] as { embedding: unknown[] }).embedding).length;
        text = JSON.stringify(json);
      }
      res.status(200).type('application/json').send(text);
      const content = capture ? (text.length <= g.maxAuditContentBytes ? json : text.slice(0, g.maxAuditContentBytes)) : undefined;
      return { done: true, usage, outcome: 'ok', content, ttft };
    } finally {
      clearTimeout(timer);
    }
  }

  /**
   * One streamed attempt on a backend (fix-long-thread-model-calls D2). The headers are already out (EventStream), so
   * the attempt can only fail over until its first byte. Its limits: the first byte (headers included) within the
   * call's first-byte limit; then each chunk within streamIdleTimeoutMs and the whole stream within streamMaxMs. The
   * request timeout does not apply here. A limit after the first byte cuts the stream; it is not an error event.
   */
  async #stream(b: Backend, call: Call): Promise<AttemptResult> {
    const { path, payload, estimate, capture, signal: clientAbort } = call;
    const out = call.out!;
    const g = this.#cfg.gateway;
    const ctl = new AbortController(); // aborted by this attempt's limits
    const signal = AbortSignal.any([clientAbort, ctl.signal]);
    const limitFired = () => ctl.signal.aborted && ctl.signal.reason instanceof StreamLimit;
    let firstTimer: NodeJS.Timeout | undefined;
    let idleTimer: NodeJS.Timeout | undefined;
    let maxTimer: NodeJS.Timeout | undefined;
    const t0 = Date.now();
    firstTimer = setTimeout(() => ctl.abort(new StreamLimit('first-byte')), call.firstByteMs);
    try {
      let upstreamRes: globalThis.Response;
      try {
        // Only the gateway's own credential is sent; nothing of the client's request headers is forwarded.
        upstreamRes = await fetch(b.endpoint(path), {
          method: 'POST',
          headers: { 'content-type': 'application/json', authorization: `Bearer ${b.apiKey}`, accept: 'text/event-stream' },
          body: payload,
          signal,
        });
      } catch (err) {
        if (clientAbort.aborted) throw err;
        if (limitFired()) return { done: false, error: timeoutErr() };
        this.#log(`${b} unreachable: ${(err as Error).name}`);
        this.pool.markUnhealthy(b);
        return { done: false, error: unavailable() };
      }

      if (!upstreamRes.ok) {
        const snippet = (await upstreamRes.text().catch(() => '')).slice(0, 8192);
        if (clientAbort.aborted) throw new Error('client aborted');
        if (limitFired()) return { done: false, error: timeoutErr() };
        const { error, retry } = this.#classify(b, upstreamRes, snippet);
        if (retry) return { done: false, error };
        return { done: true, usage: zero(), outcome: 'rejected:' + error.code.toLowerCase(), error };
      }

      const reader = upstreamRes.body!.getReader();
      let first: ReadableStreamReadResult<Uint8Array>;
      try {
        first = await raceAbort(reader.read(), signal);
      } catch (err) {
        if (clientAbort.aborted) throw err;
        if (limitFired()) return { done: false, error: timeoutErr() };
        this.#log(`${b} failed before the first byte of a stream`);
        return { done: false, error: unavailable() };
      }
      if (first.done || !first.value?.length) {
        this.#log(`${b} closed a stream before the first byte`);
        this.pool.markUnhealthy(b);
        return { done: false, error: unavailable() };
      }
      // Prompt Cache Audit (D7): upstream request start to the first upstream byte.
      const ttft = Date.now() - t0;
      clearTimeout(firstTimer);
      firstTimer = undefined;

      // After the first byte: the whole stream is bounded by the maximum, and each gap by the idle limit.
      maxTimer = setTimeout(() => ctl.abort(new StreamLimit('max')), g.streamMaxMs);
      const armIdle = () => {
        clearTimeout(idleTimer);
        idleTimer = setTimeout(() => ctl.abort(new StreamLimit('idle')), g.streamIdleTimeoutMs);
      };
      armIdle();

      const scanner = new SseScanner(capture, g.maxAuditContentBytes);
      let chunk: ReadableStreamReadResult<Uint8Array> = first;
      let aborted = false;
      try {
        while (!chunk.done) {
          const buf = Buffer.from(chunk.value);
          scanner.feed(buf);
          out.write(buf);
          armIdle();
          chunk = await raceAbort(reader.read(), signal);
        }
      } catch {
        aborted = clientAbort.aborted;
        if (!aborted) this.#log(`${b} cut a stream${limitFired() ? ` (${(ctl.signal.reason as StreamLimit).limit} limit)` : ''}`);
        await reader.cancel().catch(() => undefined);
      }
      // A stream without [DONE] or a finish reason was cut: the client sees it as truncated.
      const complete = scanner.sawDone || scanner.sawFinish;
      const approxOut = Math.floor(scanner.bytes / 16);
      const usage = scanner.usage && scanner.usage.total > 0 ? scanner.usage : { prompt: estimate, completion: approxOut, reasoning: 0, total: estimate + approxOut, cached: null };
      return {
        done: true,
        usage,
        outcome: aborted ? 'client-aborted' : complete ? 'ok' : 'truncated',
        content: capture ? { content: scanner.content, reasoning_content: scanner.reasoning || null } : undefined,
        ttft,
      };
    } finally {
      clearTimeout(firstTimer);
      clearTimeout(idleTimer);
      clearTimeout(maxTimer);
    }
  }

  /** Maps an upstream error to the gateway's vocabulary without any upstream text. Returns whether to try another backend. */
  #classify(b: Backend, r: globalThis.Response, snippet: string): { error: GatewayError; retry: boolean } {
    const s = r.status;
    if (s === 429) {
      const ra = Number(r.headers.get('retry-after'));
      const retry = Math.min(300, Math.max(1, Number.isFinite(ra) && ra > 0 ? Math.ceil(ra) : 5));
      return { error: new GatewayError(429, 'RATE_LIMIT', 'The model server is busy.', { reason: 'rate', retry_after_s: retry }), retry: true };
    }
    if (s === 400 || s === 413 || s === 422) {
      const ctx = /context[_ ]length|maximum context|too many tokens/i.test(snippet);
      return ctx
        ? { error: new GatewayError(400, 'CONTEXT_LENGTH', "The request is larger than the model's context window.", { reason: 'context window' }), retry: false }
        : { error: new GatewayError(400, 'INVALID_REQUEST', 'The model server rejected the request.'), retry: false };
    }
    if (s === 401 || s === 403) {
      this.#log(`${b} rejected the gateway's credential (${s})`);
      this.pool.markUnhealthy(b);
      return { error: unavailable(), retry: true };
    }
    if (s === 502 || s === 503 || s === 504) this.pool.markUnhealthy(b);
    else this.#log(`${b} answered ${s}`);
    return { error: unavailable(), retry: true };
  }
}

export function gatewayRoutes(gw: Gateway): Router {
  const r = Router();
  r.get('/llm/v1/models', (req, res) => gw.listModels(req, res));
  r.post('/llm/v1/chat/completions', (req, res) => gw.handle(req, res, 'chat'));
  r.post('/llm/v1/embeddings', (req, res) => gw.handle(req, res, 'embeddings'));
  r.post('/llm/v1/rerank', (req, res) => gw.handle(req, res, 'rerank'));
  r.post('/llm/v1/images/generations', (req, res) => gw.handle(req, res, 'images'));
  return r;
}
