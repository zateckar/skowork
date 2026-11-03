// The mock: state, faults, request log, token keys and the two HTTP surfaces (Graph with OAuth, and the control API on
// its own port). createMock() returns it unstarted; listen() binds both on the loopback interface.
import express from 'express';
import type { Request, Response, NextFunction } from 'express';
import { createServer, type Server } from 'node:http';
import type { AddressInfo } from 'node:net';
import { readFileSync } from 'node:fs';
import { generateKeyPair, exportJWK, type CryptoKey, type JWK } from 'jose';
import { MockState, type FixtureSet } from './state.ts';
import { oauthRouter } from './oauth.ts';
import { graphRouter } from './graph.ts';
import { controlRouter } from './control.ts';
import defaultFixtures from '../fixtures/default.json' with { type: 'json' };

export interface Fault {
  id: number;
  method?: string;
  path?: string; // a substring of the request path, or /regex/
  kind: 'throttle' | 'server_error' | 'delta_expired' | 'slow' | 'malformed';
  remaining: number;
  retry_after?: number;
  delay_ms?: number;
  status?: number;
}
export interface OAuthFault { id: number; user?: string; kind: 'interaction_required' | 'consent_required' | 'invalid_grant' | 'expired_token'; step?: 'authorize' | 'token' | 'refresh'; remaining: number }
export interface LogEntry { at: string; surface: 'graph' | 'oauth'; method: string; path: string; query: Record<string, unknown>; status: number; user?: string; fault?: string }
export interface MockOptions { seed?: string; fixtures?: string; anchor?: string; host?: string }

export function loadFixtures(name: string | undefined): FixtureSet {
  if (!name || name === 'default') return defaultFixtures as unknown as FixtureSet;
  return JSON.parse(readFileSync(name, 'utf8')) as FixtureSet;
}

export class GraphMock {
  state: MockState;
  fixturesName: string;
  faults: Fault[] = [];
  oauthFaults: OAuthFault[] = [];
  requests: LogEntry[] = [];
  oauthConfig = { access_token_lifetime_s: 3600, refresh_token_lifetime_s: 90 * 86400 };
  codes = new Map<string, { user: string; clientId: string; redirectUri: string; challenge: string; scope: string; exp: number; nonce?: string }>();
  refreshTokens = new Map<string, { user: string; clientId: string; scope: string; exp: number }>();
  /** Delta and paging continuations by token id (they stay on the server, like Graph's opaque tokens). */
  continuations = new Map<string, unknown>();
  #faultId = 0;
  #tokenCounter = 0;
  privateKey!: CryptoKey;
  jwk!: JWK;
  host: string;
  graphBase = '';
  controlBase = '';
  #servers: Server[] = [];

  constructor(opts: MockOptions = {}) {
    this.host = opts.host ?? '127.0.0.1';
    this.fixturesName = opts.fixtures ?? 'default';
    this.state = new MockState(loadFixtures(this.fixturesName), opts.seed ?? 'workbench', opts.anchor);
  }

  async init(): Promise<void> {
    const { privateKey, publicKey } = await generateKeyPair('RS256', { extractable: true });
    this.privateKey = privateKey;
    this.jwk = { ...(await exportJWK(publicKey)), kid: 'mock-key-1', use: 'sig', alg: 'RS256' };
  }

  now(): Date {
    return new Date();
  }
  issuer(tenant: string): string {
    return `${this.graphBase}/${tenant}/v2.0`;
  }
  nextToken(prefix: string): string {
    return `${prefix}.${this.state.seed}.${++this.#tokenCounter}.${Math.random().toString(36).slice(2)}`;
  }

  reset(opts: { seed?: string; fixtures?: string; anchor?: string }): void {
    const generation = this.state.generation + 1;
    this.fixturesName = opts.fixtures ?? this.fixturesName;
    this.state = new MockState(loadFixtures(this.fixturesName), opts.seed ?? this.state.seed, opts.anchor);
    this.state.generation = generation;
    this.faults = [];
    this.oauthFaults = [];
    this.requests = [];
    this.continuations.clear();
  }

  addFault(f: Omit<Fault, 'id'>): Fault {
    const fault = { ...f, id: ++this.#faultId };
    this.faults.push(fault);
    return fault;
  }
  addOAuthFault(f: Omit<OAuthFault, 'id'>): OAuthFault {
    const fault = { ...f, id: ++this.#faultId };
    this.oauthFaults.push(fault);
    return fault;
  }
  /** Takes one use of the first matching fault of these kinds. */
  takeFault(req: Request, kinds: Fault['kind'][]): Fault | undefined {
    const path = req.path;
    const f = this.faults.find((x) => {
      if (x.remaining <= 0 || !kinds.includes(x.kind)) return false;
      if (x.method && x.method.toUpperCase() !== req.method) return false;
      if (x.path) {
        const re = /^\/(.*)\/$/.exec(x.path);
        if (re ? !new RegExp(re[1]!).test(path) : !path.includes(x.path)) return false;
      }
      return true;
    });
    if (f) f.remaining--;
    return f;
  }
  takeOAuthFault(user: string | undefined, step: 'authorize' | 'token' | 'refresh', kinds: OAuthFault['kind'][]): OAuthFault | undefined {
    const f = this.oauthFaults.find((x) => x.remaining > 0 && kinds.includes(x.kind) && (!x.step || x.step === step) && (!x.user || !user || this.state.user(x.user)?.key === this.state.user(user)?.key));
    if (f) f.remaining--;
    return f;
  }

  log(e: Omit<LogEntry, 'at'>): void {
    this.requests.push({ at: this.now().toISOString(), ...e });
  }

  graphApp(): express.Express {
    const app = express();
    app.disable('x-powered-by');
    app.set('query parser', 'simple');
    app.use(express.json({ limit: '40mb' }));
    app.use(express.urlencoded({ extended: false }));
    app.use(oauthRouter(this));
    app.use('/v1.0', graphRouter(this));
    app.use((req: Request, res: Response) => {
      res.status(404).json({ error: { code: 'NotFound', message: `No route ${req.method} ${req.path}` } });
    });
    app.use((err: Error & { status?: number; type?: string }, _req: Request, res: Response, _next: NextFunction) => {
      res.status(err.status ?? 500).json({ error: { code: err.type === 'entity.parse.failed' ? 'BadRequest' : 'InternalServerError', message: err.message } });
    });
    return app;
  }

  controlApp(): express.Express {
    const app = express();
    app.disable('x-powered-by');
    app.use(express.json({ limit: '40mb' }));
    app.use('/_mock', controlRouter(this));
    return app;
  }

  async listen(graphPort = 0, controlPort = 0): Promise<void> {
    if (!this.privateKey) await this.init();
    const bind = (app: express.Express, port: number) =>
      new Promise<number>((resolve, reject) => {
        const s = createServer(app);
        s.once('error', reject);
        s.listen(port, this.host, () => resolve((s.address() as AddressInfo).port));
        this.#servers.push(s);
      });
    const g = await bind(this.graphApp(), graphPort);
    this.graphBase = `http://${this.host}:${g}`;
    const c = await bind(this.controlApp(), controlPort);
    this.controlBase = `http://${this.host}:${c}`;
  }

  async close(): Promise<void> {
    await Promise.all(this.#servers.map((s) => new Promise<void>((r) => { s.closeAllConnections(); s.close(() => r()); })));
    this.#servers = [];
  }
}

export async function createMock(opts: MockOptions = {}): Promise<GraphMock> {
  const m = new GraphMock(opts);
  await m.init();
  return m;
}
