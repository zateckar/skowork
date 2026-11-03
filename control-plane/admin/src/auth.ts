// Browser sign-in for the admin console: Authorization Code with PKCE against the organization Keycloak (public client).
// Tokens live in memory; the refresh token is kept in sessionStorage so a reload of the tab stays signed in.

interface AdminConfig {
  discovery_url: string;
  client_id: string;
  redirect_uri: string;
  scope: string;
}
interface Discovery {
  authorization_endpoint: string;
  token_endpoint: string;
  end_session_endpoint?: string;
}

let cfg: AdminConfig | null = null;
let disc: Discovery | null = null;
let accessToken: string | null = null;
let expiresAt = 0;
let refreshing: Promise<void> | null = null;

const b64url = (bytes: ArrayBuffer | Uint8Array) =>
  btoa(String.fromCharCode(...new Uint8Array(bytes))).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
const random = (n: number) => b64url(crypto.getRandomValues(new Uint8Array(n)));

async function load(): Promise<void> {
  if (cfg && disc) return;
  cfg = (await (await fetch('/api/v1/public/admin-config')).json()) as AdminConfig;
  disc = (await (await fetch(cfg.discovery_url)).json()) as Discovery;
}

export async function signIn(): Promise<void> {
  await load();
  const verifier = random(48);
  const state = random(16);
  sessionStorage.setItem('pkce', JSON.stringify({ verifier, state, back: location.pathname + location.hash }));
  const challenge = b64url(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(verifier)));
  const u = new URL(disc!.authorization_endpoint);
  u.search = new URLSearchParams({
    response_type: 'code', client_id: cfg!.client_id, redirect_uri: cfg!.redirect_uri, scope: cfg!.scope, state,
    code_challenge: challenge, code_challenge_method: 'S256',
  }).toString();
  location.assign(u.toString());
}

async function tokenRequest(form: Record<string, string>): Promise<void> {
  const r = await fetch(disc!.token_endpoint, { method: 'POST', body: new URLSearchParams(form) });
  const j = await r.json();
  if (!r.ok) {
    sessionStorage.removeItem('rt');
    accessToken = null;
    throw new Error(j.error_description ?? j.error ?? `token endpoint ${r.status}`);
  }
  accessToken = j.access_token;
  expiresAt = Date.now() + j.expires_in * 1000;
  if (j.refresh_token) sessionStorage.setItem('rt', j.refresh_token);
}

/** Completes a redirect back from the identity provider, or restores the session. Returns whether signed in. */
export async function init(): Promise<boolean> {
  await load();
  const q = new URLSearchParams(location.search);
  const pending = sessionStorage.getItem('pkce');
  if (q.get('code') && pending) {
    const { verifier, state, back } = JSON.parse(pending);
    sessionStorage.removeItem('pkce');
    if (q.get('state') !== state) throw new Error('sign-in state mismatch');
    await tokenRequest({ grant_type: 'authorization_code', client_id: cfg!.client_id, code: q.get('code')!, redirect_uri: cfg!.redirect_uri, code_verifier: verifier });
    history.replaceState(null, '', back || '/');
    return true;
  }
  if (sessionStorage.getItem('rt')) {
    try {
      await refresh();
      return true;
    } catch {
      return false;
    }
  }
  return false;
}

function refresh(): Promise<void> {
  refreshing ??= tokenRequest({ grant_type: 'refresh_token', client_id: cfg!.client_id, refresh_token: sessionStorage.getItem('rt') ?? '' }).finally(
    () => (refreshing = null),
  );
  return refreshing;
}

/** A fresh access token: refreshed when less than 60 s remain (single-flight). */
export async function token(): Promise<string> {
  if (!accessToken || Date.now() > expiresAt - 60_000) await refresh();
  return accessToken!;
}

export async function signOut(): Promise<void> {
  sessionStorage.removeItem('rt');
  accessToken = null;
  if (disc?.end_session_endpoint) {
    const u = new URL(disc.end_session_endpoint);
    u.search = new URLSearchParams({ client_id: cfg!.client_id, post_logout_redirect_uri: cfg!.redirect_uri.replace(/\/auth\/callback$/, '/') }).toString();
    location.assign(u.toString());
  } else location.reload();
}

export class ApiError extends Error {
  readonly status: number;
  readonly code: string;
  /** The whole error object of the response, for structured details such as policy validation errors. */
  readonly details: Record<string, unknown>;
  constructor(status: number, code: string, message: string, details: Record<string, unknown> = {}) {
    super(message);
    this.status = status;
    this.code = code;
    this.details = details;
  }
}

/** A download (export, crash dump) with the access token; saved through a temporary link. */
export async function download(path: string): Promise<void> {
  const r = await fetch(path, { headers: { authorization: `Bearer ${await token()}` } });
  if (!r.ok) {
    const j = await r.json().catch(() => ({}));
    throw new ApiError(r.status, j?.error?.code ?? `HTTP_${r.status}`, j?.error?.message ?? r.statusText, j?.error ?? {});
  }
  const name = /filename="([^"]+)"/.exec(r.headers.get('content-disposition') ?? '')?.[1] ?? 'download';
  const url = URL.createObjectURL(await r.blob());
  const a = Object.assign(document.createElement('a'), { href: url, download: name });
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 10_000);
}

/** POSTs a file as the raw request body (a Skill package import). */
export function upload<T = unknown>(path: string, file: Blob): Promise<T> {
  return api<T>('POST', path, file);
}

export async function api<T = unknown>(method: string, path: string, body?: unknown): Promise<T> {
  const raw = body instanceof Blob;
  const send = async () =>
    fetch(path, {
      method,
      headers: {
        authorization: `Bearer ${await token()}`,
        ...(body !== undefined ? { 'content-type': raw ? 'application/zip' : 'application/json' } : {}),
      },
      body: body === undefined ? undefined : raw ? body : JSON.stringify(body),
    });
  let r = await send();
  if (r.status === 401) {
    await refresh();
    r = await send();
  }
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new ApiError(r.status, j?.error?.code ?? `HTTP_${r.status}`, j?.error?.message ?? r.statusText, j?.error ?? {});
  return j as T;
}
