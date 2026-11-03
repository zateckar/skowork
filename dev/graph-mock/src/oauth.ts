// Entra-shaped OAuth endpoints (dev/graph-mock Mock OAuth Endpoints): discovery, keys, authorize with auto-consent for
// test users and PKCE, and token for the authorization_code and refresh_token grants. Tokens are RS256 JWTs signed with
// the mock's own key and naming the mock as issuer.
import { Router, type Request, type Response } from 'express';
import { createHash } from 'node:crypto';
import { SignJWT } from 'jose';
import type { GraphMock } from './mock.ts';

const KNOWN_SCOPES = ['User.Read', 'Mail.Read', 'Mail.ReadWrite', 'Mail.Send', 'Calendars.Read', 'Calendars.ReadWrite', 'MailboxSettings.Read', 'User.ReadBasic.All', 'User.Read.All'];

/** "https://graph.microsoft.com/Mail.Read offline_access" -> the Graph scopes the mock grants, and offline access. */
export function grantedScopes(scope: string): { graph: string[]; offline: boolean; openid: boolean } {
  const parts = scope.split(/\s+/).filter(Boolean);
  const graph = new Set<string>();
  for (const p of parts) {
    const s = p.replace(/^https:\/\/graph\.microsoft\.com\//i, '');
    const known = KNOWN_SCOPES.find((k) => k.toLowerCase() === s.toLowerCase());
    if (known) graph.add(known);
  }
  return { graph: [...graph], offline: parts.includes('offline_access'), openid: parts.includes('openid') };
}

function isLoopbackRedirect(uri: string): boolean {
  try {
    const u = new URL(uri);
    return u.protocol === 'http:' && ['localhost', '127.0.0.1', '[::1]'].includes(u.hostname);
  } catch {
    return false;
  }
}

function oauthError(res: Response, status: number, error: string, description: string, extra: Record<string, unknown> = {}): void {
  res.status(status).json({ error, error_description: description, error_codes: [], timestamp: new Date().toISOString(), ...extra });
}

export function oauthRouter(mock: GraphMock): Router {
  const r = Router();

  r.get('/:tenant/v2.0/.well-known/openid-configuration', (req, res) => {
    const t = String(req.params.tenant);
    const base = `${mock.graphBase}/${t}`;
    mock.log({ surface: 'oauth', method: req.method, path: req.path, query: req.query, status: 200 });
    res.json({
      issuer: mock.issuer(t),
      authorization_endpoint: `${base}/oauth2/v2.0/authorize`,
      token_endpoint: `${base}/oauth2/v2.0/token`,
      jwks_uri: `${base}/discovery/v2.0/keys`,
      response_types_supported: ['code'],
      code_challenge_methods_supported: ['S256'],
      grant_types_supported: ['authorization_code', 'refresh_token'],
      scopes_supported: ['openid', 'profile', 'email', 'offline_access'],
      token_endpoint_auth_methods_supported: ['none'],
      tenant_region_scope: 'EU',
    });
  });

  r.get('/:tenant/discovery/v2.0/keys', (_req, res) => {
    res.json({ keys: [mock.jwk] });
  });

  r.get('/:tenant/oauth2/v2.0/authorize', (req: Request, res: Response) => {
    const q = req.query as Record<string, string | undefined>;
    const log = (status: number, user?: string, fault?: string) => mock.log({ surface: 'oauth', method: 'GET', path: req.path, query: { ...q }, status, user, fault });
    const redirect = q.redirect_uri ?? '';
    if (!q.client_id || !isLoopbackRedirect(redirect)) {
      log(400);
      return oauthError(res, 400, 'invalid_request', 'AADSTS50011: The redirect URI must be an http loopback URI and client_id is required.');
    }
    const back = (params: Record<string, string>) => {
      const u = new URL(redirect);
      for (const [k, v] of Object.entries(params)) u.searchParams.set(k, v);
      if (q.state !== undefined) u.searchParams.set('state', q.state);
      res.redirect(302, u.toString());
    };
    if (q.response_type !== 'code' || q.code_challenge_method !== 'S256' || !q.code_challenge) {
      log(302);
      return back({ error: 'invalid_request', error_description: 'AADSTS9002325: Proof Key for Code Exchange is required (code, S256).' });
    }
    const user = (q.login_hint && mock.state.user(q.login_hint)) || mock.state.defaultUser();
    const fault = mock.takeOAuthFault(user.key, 'authorize', ['interaction_required', 'consent_required']);
    if (fault) {
      log(302, user.key, fault.kind);
      return back({ error: fault.kind, error_description: `AADSTS65001: injected ${fault.kind}` });
    }
    if (q.prompt === 'none' && !user) {
      log(302);
      return back({ error: 'login_required', error_description: 'AADSTS50058: no signed-in user.' });
    }
    const code = mock.nextToken('code');
    mock.codes.set(code, { user: user.key, clientId: q.client_id, redirectUri: redirect, challenge: q.code_challenge, scope: q.scope ?? '', exp: Date.now() + 10 * 60_000, nonce: q.nonce });
    log(302, user.key);
    back({ code, session_state: 'mock-session' });
  });

  r.post('/:tenant/oauth2/v2.0/token', async (req: Request, res: Response) => {
    const b = (req.body ?? {}) as Record<string, string | undefined>;
    const tenant = String(req.params.tenant);
    const log = (status: number, user?: string, fault?: string) =>
      mock.log({ surface: 'oauth', method: 'POST', path: req.path, query: { grant_type: b.grant_type, scope: b.scope }, status, user, fault });
    let user: string, clientId: string, scope: string, nonce: string | undefined;
    if (b.grant_type === 'authorization_code') {
      const c = b.code ? mock.codes.get(b.code) : undefined;
      if (b.code) mock.codes.delete(b.code); // single use
      if (!c || c.exp < Date.now()) { log(400); return oauthError(res, 400, 'invalid_grant', 'AADSTS70008: The provided authorization code is expired or was already used.'); }
      if (c.clientId !== b.client_id || c.redirectUri !== b.redirect_uri) { log(400, c.user); return oauthError(res, 400, 'invalid_grant', 'AADSTS50148: client_id or redirect_uri does not match the authorization request.'); }
      const challenge = createHash('sha256').update(b.code_verifier ?? '').digest('base64url');
      if (!b.code_verifier || challenge !== c.challenge) { log(400, c.user); return oauthError(res, 400, 'invalid_grant', 'AADSTS501481: The Code_Verifier does not match the code_challenge supplied in the authorization request.'); }
      const f = mock.takeOAuthFault(c.user, 'token', ['interaction_required', 'consent_required', 'invalid_grant']);
      if (f) { log(400, c.user, f.kind); return oauthError(res, 400, f.kind, `AADSTS65001: injected ${f.kind}`); }
      user = c.user;
      clientId = c.clientId;
      scope = b.scope || c.scope;
      nonce = c.nonce;
    } else if (b.grant_type === 'refresh_token') {
      const rt = b.refresh_token ? mock.refreshTokens.get(b.refresh_token) : undefined;
      if (!rt || rt.exp < Date.now() || rt.clientId !== b.client_id) { log(400); return oauthError(res, 400, 'invalid_grant', 'AADSTS700082: The refresh token has expired or is invalid.'); }
      const f = mock.takeOAuthFault(rt.user, 'refresh', ['interaction_required', 'consent_required', 'invalid_grant']);
      if (f) {
        if (f.kind === 'invalid_grant') mock.refreshTokens.delete(b.refresh_token!);
        log(400, rt.user, f.kind);
        return oauthError(res, 400, f.kind, `AADSTS50076: injected ${f.kind}`, f.kind === 'interaction_required' ? { suberror: 'basic_action' } : {});
      }
      mock.refreshTokens.delete(b.refresh_token!); // rotated
      user = rt.user;
      clientId = rt.clientId;
      scope = b.scope || rt.scope;
    } else {
      log(400);
      return oauthError(res, 400, 'unsupported_grant_type', 'AADSTS70003: only authorization_code and refresh_token are supported.');
    }
    const u = mock.state.user(user)!;
    const granted = grantedScopes(scope);
    const now = Math.floor(Date.now() / 1000);
    const life = mock.oauthConfig.access_token_lifetime_s;
    const expired = mock.takeOAuthFault(user, 'token', ['expired_token']);
    const access = await new SignJWT({ scp: granted.graph.join(' '), oid: u.id, upn: u.upn, name: u.displayName, tid: tenant, appid: clientId, ver: '2.0', mock: true, jti: mock.nextToken('at') })
      .setProtectedHeader({ alg: 'RS256', kid: 'mock-key-1', typ: 'JWT' })
      .setIssuer(mock.issuer(tenant))
      .setAudience('https://graph.microsoft.com')
      .setSubject(u.id)
      .setIssuedAt(now)
      .setExpirationTime(expired ? now - 60 : now + life)
      .sign(mock.privateKey);
    const body: Record<string, unknown> = { token_type: 'Bearer', scope: granted.graph.map((s) => `https://graph.microsoft.com/${s}`).join(' '), expires_in: life, ext_expires_in: life, access_token: access };
    if (granted.offline) {
      const refresh = mock.nextToken('refresh');
      mock.refreshTokens.set(refresh, { user, clientId, scope, exp: Date.now() + mock.oauthConfig.refresh_token_lifetime_s * 1000 });
      body.refresh_token = refresh;
    }
    if (granted.openid) {
      body.id_token = await new SignJWT({ oid: u.id, preferred_username: u.upn, name: u.displayName, tid: tenant, ...(nonce ? { nonce } : {}) })
        .setProtectedHeader({ alg: 'RS256', kid: 'mock-key-1', typ: 'JWT' })
        .setIssuer(mock.issuer(tenant))
        .setAudience(clientId)
        .setSubject(u.id)
        .setIssuedAt(now)
        .setExpirationTime(now + 3600)
        .sign(mock.privateKey);
    }
    log(200, user, expired ? 'expired_token' : undefined);
    res.set('cache-control', 'no-store').json(body);
  });

  return r;
}
