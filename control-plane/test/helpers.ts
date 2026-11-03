// Test helpers: an in-test token issuer (the real identity provider cannot give a second user or an expired token on
// demand), a control plane on a random port with an in-memory database, and a WebSocket client for the push channel.
import { createServer, type IncomingMessage, type Server, type ServerResponse } from 'node:http';
import { randomUUID } from 'node:crypto';
import { exportJWK, generateKeyPair, SignJWT, type CryptoKey } from 'jose';
import WebSocket from 'ws';
import { loadConfig, type Config } from '../src/config.ts';
import { createControlPlane, type ControlPlane } from '../src/app.ts';

export const TEST_ISSUER = 'https://idp.test/realms/workbench';
export const CLIENT_ID = 'workbench-desktop-test';

export interface Issuer {
  sign(claims?: Record<string, unknown>, opts?: { lifetimeS?: number; issuedAgoS?: number; issuer?: string }): Promise<string>;
  jwks: { keys: object[] };
}

export async function makeIssuer(kid = 'k1'): Promise<Issuer & { key: CryptoKey }> {
  const { privateKey, publicKey } = await generateKeyPair('ES256', { extractable: true });
  const jwk = { ...(await exportJWK(publicKey)), kid, alg: 'ES256', use: 'sig' };
  return {
    key: privateKey,
    jwks: { keys: [jwk] },
    async sign(claims = {}, opts = {}) {
      const now = Math.floor(Date.now() / 1000) - (opts.issuedAgoS ?? 0);
      const user = String(claims.preferred_username ?? 'dev1');
      return new SignJWT({
        sub: `sub-${user}`,
        preferred_username: user,
        azp: CLIENT_ID,
        aud: [CLIENT_ID, 'eai'],
        sid: `sid-${user}`,
        email: `${user}@example.test`,
        name: user,
        roles: [],
        ...claims,
      })
        .setProtectedHeader({ alg: 'ES256', kid, typ: 'JWT' })
        .setIssuer(opts.issuer ?? TEST_ISSUER)
        .setIssuedAt(now)
        .setExpirationTime(now + (opts.lifetimeS ?? 300))
        .sign(privateKey);
    },
  };
}

export interface Harness {
  cp: ControlPlane;
  base: string;
  issuer: Issuer;
  cfg: Config;
  token(user?: string, claims?: Record<string, unknown>, opts?: { lifetimeS?: number; issuedAgoS?: number }): Promise<string>;
  call(method: string, path: string, token: string | null, body?: unknown, headers?: Record<string, string>): Promise<{ status: number; body: any; headers: Headers }>;
  close(): Promise<void>;
}

export async function startHarness(patch: (c: Config) => void = () => {}): Promise<Harness> {
  const issuer = await makeIssuer();
  const cfg = loadConfig({
    WORKBENCH_ENV: 'test',
    WORKBENCH_DB: ':memory:',
    OIDC_CLIENT_ID: CLIENT_ID,
    WORKBENCH_TEST_ISSUER: TEST_ISSUER,
    WORKBENCH_TEST_ISSUER_JWKS: JSON.stringify(issuer.jwks),
    WORKBENCH_BOOTSTRAP_ADMINS: 'admin1',
  });
  patch(cfg);
  const cp = createControlPlane(cfg, () => {});
  const port = await cp.listen(0, '127.0.0.1');
  const base = `http://127.0.0.1:${port}`;
  return {
    cp,
    base,
    issuer,
    cfg,
    token: (user = 'dev1', claims = {}, opts = {}) => issuer.sign({ preferred_username: user, ...claims }, opts),
    async call(method, path, token, body, headers = {}) {
      const res = await fetch(base + path, {
        method,
        headers: { ...(token ? { authorization: `Bearer ${token}` } : {}), ...(body !== undefined ? { 'content-type': 'application/json' } : {}), ...headers },
        body: body === undefined ? undefined : JSON.stringify(body),
      });
      const text = await res.text();
      let parsed: unknown = text;
      try {
        parsed = JSON.parse(text);
      } catch {
        /* keep text */
      }
      return { status: res.status, body: parsed, headers: res.headers };
    },
    close: () => cp.close(),
  };
}

export async function register(h: Harness, token: string, extra: Record<string, unknown> = {}) {
  const installation_id = randomUUID();
  const r = await h.call('POST', '/api/v1/fleet/register', token, {
    installation_id, machine_name: 'PC-1', version: '1.0.0', channel: 'stable', commit: 'abc', os_build: '10.0.22631', hardware_class: 'cpu:8c', ...extra,
  });
  return { installation_id, ...r };
}

/** A push client that collects messages and the close code. */
export class PushClient {
  readonly ws: WebSocket;
  readonly messages: any[] = [];
  closeCode: number | null = null;
  readonly opened: Promise<void>;
  readonly closed: Promise<number>;
  #waiters: (() => void)[] = [];

  constructor(h: Harness, installationId: string, token: string, lastAck = 0) {
    this.ws = new WebSocket(`${h.base.replace('http', 'ws')}/api/v1/fleet/push?installation_id=${installationId}&last_ack=${lastAck}`, {
      headers: { authorization: `Bearer ${token}` },
    });
    this.opened = new Promise((resolve, reject) => {
      this.ws.once('open', () => resolve());
      this.ws.once('unexpected-response', (_req, res) => reject(new Error(`handshake ${res.statusCode}`)));
      this.ws.once('error', reject);
    });
    this.closed = new Promise((resolve) => this.ws.once('close', (code) => resolve((this.closeCode = code))));
    this.ws.on('message', (d) => {
      this.messages.push(JSON.parse(d.toString()));
      for (const w of this.#waiters.splice(0)) w();
    });
  }
  send(m: unknown) {
    this.ws.send(JSON.stringify(m));
  }
  async waitFor(pred: (m: any) => boolean, ms = 3000): Promise<any> {
    const deadline = Date.now() + ms;
    for (;;) {
      const hit = this.messages.find(pred);
      if (hit) return hit;
      const left = deadline - Date.now();
      if (left <= 0) throw new Error(`timed out; got ${JSON.stringify(this.messages)}`);
      await new Promise<void>((r) => {
        const t = setTimeout(r, left);
        this.#waiters.push(() => (clearTimeout(t), r()));
      });
    }
  }
  close() {
    this.ws.terminate();
  }
}

export const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

export async function waitUntil(pred: () => boolean | Promise<boolean>, ms = 3000): Promise<void> {
  const deadline = Date.now() + ms;
  while (!(await pred())) {
    if (Date.now() > deadline) throw new Error('condition not met in time');
    await sleep(20);
  }
}

/** A 1×1 PNG, as the upstream's image endpoint returns images (base64, no URL). */
export const TINY_PNG = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==';

/** An in-test OpenAI-compatible upstream, used only for faults the real API cannot produce on demand. */
export interface FakeUpstream {
  url: string;
  requests: { path: string; headers: IncomingMessage['headers']; body: any }[];
  handler: (path: string, body: any, res: ServerResponse) => void;
  close(): Promise<void>;
}

export async function fakeUpstream(): Promise<FakeUpstream> {
  const f: FakeUpstream = {
    url: '',
    requests: [],
    handler: (path, body, res) => {
      if (path.endsWith('/chat/completions') && body.stream) {
        res.writeHead(200, { 'content-type': 'text/event-stream' });
        res.write(`data: ${JSON.stringify({ choices: [{ delta: { content: 'Hello' } }] })}\n\n`);
        res.write(`data: ${JSON.stringify({ choices: [{ delta: {}, finish_reason: 'stop' }] })}\n\n`);
        res.write(`data: ${JSON.stringify({ choices: [], usage: { prompt_tokens: 10, completion_tokens: 2, total_tokens: 12 } })}\n\n`);
        res.end('data: [DONE]\n\n');
      } else if (path.endsWith('/chat/completions')) {
        json(res, 200, { choices: [{ message: { role: 'assistant', content: 'Hello' }, finish_reason: 'stop' }], usage: { prompt_tokens: 10, completion_tokens: 2, total_tokens: 12 } });
      } else if (path.endsWith('/embeddings')) {
        const inputs = Array.isArray(body.input) ? body.input : [body.input];
        json(res, 200, { model: body.model, data: inputs.map((_: unknown, i: number) => ({ index: i, embedding: [0.1, 0.2, 0.3, 0.4] })), usage: { prompt_tokens: 5, total_tokens: 5 } });
      } else if (path.endsWith('/rerank')) {
        json(res, 200, { results: body.documents.map((_: unknown, i: number) => ({ index: i, relevance_score: 1 - i / 100 })), meta: { billed_units: { input_tokens: 7 } } });
      } else if (path.endsWith('/images/generations')) {
        json(res, 200, { created: 1, output_format: 'png', data: Array.from({ length: body.n ?? 1 }, () => ({ b64_json: TINY_PNG, revised_prompt: '' })), usage: { total_tokens: 0 } });
      } else if (path.endsWith('/models')) {
        json(res, 200, { data: [] });
      } else json(res, 404, {});
    },
    close: async () => {},
  };
  const server: Server = createServer((req, res) => {
    let raw = '';
    req.on('data', (c) => (raw += c));
    req.on('end', () => {
      let body: any = {};
      try {
        body = raw ? JSON.parse(raw) : {};
      } catch {
        /* ignore */
      }
      f.requests.push({ path: req.url ?? '', headers: req.headers, body });
      f.handler(req.url ?? '', body, res);
    });
  });
  await new Promise<void>((r) => server.listen(0, '127.0.0.1', () => r()));
  const addr = server.address();
  f.url = `http://127.0.0.1:${typeof addr === 'object' && addr ? addr.port : 0}/v1`;
  f.close = () => new Promise<void>((r) => (server.closeAllConnections(), server.close(() => r())));
  return f;
}

/** An in-test remote MCP server (Streamable HTTP) for catalog discovery; its tool list can change between calls. */
export interface FakeMcp {
  url: string;
  tools: { name: string; description?: string; annotations?: object }[];
  /** Answer with an event stream instead of plain JSON. */
  sse: boolean;
  authorizations: (string | undefined)[];
  close(): Promise<void>;
}

export async function fakeMcp(): Promise<FakeMcp> {
  const f: FakeMcp = { url: '', tools: [], sse: false, authorizations: [], close: async () => {} };
  const server: Server = createServer((req, res) => {
    let raw = '';
    req.on('data', (c) => (raw += c));
    req.on('end', () => {
      if (req.method !== 'POST') return json(res, 405, {});
      f.authorizations.push(req.headers.authorization);
      const msg = JSON.parse(raw);
      if (msg.id === undefined) {
        res.writeHead(202).end();
        return;
      }
      let result: unknown;
      if (msg.method === 'initialize') {
        result = { protocolVersion: '2025-06-18', capabilities: { tools: {} }, serverInfo: { name: 'fake', version: '1' } };
        res.setHeader('mcp-session-id', 'session-1');
      } else if (msg.method === 'tools/list') {
        // Two tools per page, to exercise paging.
        const start = Number(msg.params?.cursor ?? 0);
        const page = f.tools.slice(start, start + 2).map((t) => ({ inputSchema: { type: 'object' }, ...t }));
        result = { tools: page, ...(start + 2 < f.tools.length ? { nextCursor: String(start + 2) } : {}) };
      } else {
        return json(res, 200, { jsonrpc: '2.0', id: msg.id, error: { code: -32601, message: 'method not found' } });
      }
      const answer = { jsonrpc: '2.0', id: msg.id, result };
      if (f.sse) {
        res.writeHead(200, { 'content-type': 'text/event-stream' });
        res.end(`event: message\ndata: ${JSON.stringify(answer)}\n\n`);
      } else json(res, 200, answer);
    });
  });
  await new Promise<void>((r) => server.listen(0, '127.0.0.1', () => r()));
  const addr = server.address();
  f.url = `http://127.0.0.1:${typeof addr === 'object' && addr ? addr.port : 0}/mcp`;
  f.close = () => new Promise<void>((r) => (server.closeAllConnections(), server.close(() => r())));
  return f;
}

export function json(res: ServerResponse, status: number, body: unknown) {
  res.writeHead(status, { 'content-type': 'application/json' });
  res.end(JSON.stringify(body));
}
