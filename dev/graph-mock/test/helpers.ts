import { createHash, randomBytes } from 'node:crypto';
import { GraphMock, type MockOptions } from '../src/mock.ts';

export const ALL_SCOPES = 'https://graph.microsoft.com/User.Read https://graph.microsoft.com/Mail.ReadWrite https://graph.microsoft.com/Mail.Send https://graph.microsoft.com/Calendars.ReadWrite https://graph.microsoft.com/MailboxSettings.Read offline_access openid profile';
export const REDIRECT = 'http://localhost:53682/';

export async function startMock(opts: MockOptions = {}): Promise<GraphMock> {
  const m = new GraphMock(opts);
  await m.listen(0, 0);
  return m;
}

export interface Tokens { access_token: string; refresh_token?: string; scope: string; expires_in: number }

/** The authorization code flow with PKCE, the way the client runs it (the redirect is read instead of followed). */
export async function signIn(m: GraphMock, opts: { user?: string; scope?: string; verifier?: string } = {}): Promise<{ status: number; body: Record<string, unknown>; tokens?: Tokens; redirect?: URL }> {
  const verifier = randomBytes(32).toString('base64url');
  const challenge = createHash('sha256').update(verifier).digest('base64url');
  const q = new URLSearchParams({ client_id: 'workbench-dev', response_type: 'code', redirect_uri: REDIRECT, scope: opts.scope ?? ALL_SCOPES, state: 's1', code_challenge: challenge, code_challenge_method: 'S256' });
  if (opts.user) q.set('login_hint', opts.user);
  const a = await fetch(`${m.graphBase}/mock-tenant/oauth2/v2.0/authorize?${q}`, { redirect: 'manual' });
  const redirect = new URL(a.headers.get('location') ?? 'http://invalid/');
  const code = redirect.searchParams.get('code');
  if (!code) return { status: a.status, body: Object.fromEntries(redirect.searchParams), redirect };
  const t = await fetch(`${m.graphBase}/mock-tenant/oauth2/v2.0/token`, {
    method: 'POST',
    headers: { 'content-type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({ grant_type: 'authorization_code', code, redirect_uri: REDIRECT, client_id: 'workbench-dev', code_verifier: opts.verifier ?? verifier }),
  });
  const body = (await t.json()) as Record<string, unknown>;
  return { status: t.status, body, tokens: t.ok ? (body as unknown as Tokens) : undefined, redirect };
}

export async function refresh(m: GraphMock, refreshToken: string): Promise<{ status: number; body: Record<string, unknown> }> {
  const t = await fetch(`${m.graphBase}/mock-tenant/oauth2/v2.0/token`, {
    method: 'POST',
    headers: { 'content-type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({ grant_type: 'refresh_token', refresh_token: refreshToken, client_id: 'workbench-dev', scope: ALL_SCOPES }),
  });
  return { status: t.status, body: (await t.json()) as Record<string, unknown> };
}

export function graph(m: GraphMock, token: string) {
  const call = async (method: string, path: string, body?: unknown, headers: Record<string, string> = {}) => {
    const r = await fetch(path.startsWith('http') ? path : `${m.graphBase}/v1.0${path}`, {
      method,
      headers: { authorization: `Bearer ${token}`, ...(body !== undefined ? { 'content-type': 'application/json' } : {}), ...headers },
      body: body !== undefined ? JSON.stringify(body) : undefined,
    });
    const text = await r.text();
    let json: any = undefined;
    try { json = text ? JSON.parse(text) : undefined; } catch { json = undefined; }
    return { status: r.status, json, text, headers: r.headers };
  };
  return {
    get: (p: string, h?: Record<string, string>) => call('GET', p, undefined, h),
    post: (p: string, b?: unknown) => call('POST', p, b ?? {}),
    patch: (p: string, b: unknown) => call('PATCH', p, b),
    del: (p: string) => call('DELETE', p),
  };
}

export function control(m: GraphMock) {
  const call = async (method: string, path: string, body?: unknown) => {
    const r = await fetch(`${m.controlBase}/_mock${path}`, { method, headers: body !== undefined ? { 'content-type': 'application/json' } : {}, body: body !== undefined ? JSON.stringify(body) : undefined });
    const text = await r.text();
    return { status: r.status, json: text ? JSON.parse(text) : undefined };
  };
  return { get: (p: string) => call('GET', p), post: (p: string, b: unknown = {}) => call('POST', p, b), patch: (p: string, b: unknown) => call('PATCH', p, b), del: (p: string) => call('DELETE', p) };
}

/** Signs in as the user with every scope and returns a Graph client. */
export async function as(m: GraphMock, user = 'jan', scope = ALL_SCOPES) {
  const s = await signIn(m, { user, scope });
  if (!s.tokens) throw new Error('sign-in failed: ' + JSON.stringify(s.body));
  return graph(m, s.tokens.access_token);
}

/** Follows nextLink to the end; returns all items and the final deltaLink. */
export async function all(g: ReturnType<typeof graph>, first: string, headers?: Record<string, string>) {
  const items: any[] = [];
  let url: string | undefined = first;
  let deltaLink: string | undefined;
  let pages = 0;
  while (url) {
    const r = await g.get(url, headers);
    if (r.status !== 200) return { items, status: r.status, json: r.json, deltaLink, pages };
    items.push(...r.json.value);
    pages++;
    url = r.json['@odata.nextLink'];
    deltaLink = r.json['@odata.deltaLink'];
  }
  return { items, status: 200, json: undefined, deltaLink, pages };
}
