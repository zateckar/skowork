// Probe the organization Keycloak client from .env: Authorization Code + PKCE with a loopback redirect.
// Prints decoded claims and lifetimes only. Token values are never printed or written.
//   node dev/scripts/oidc-probe.mjs
import { createServer } from 'node:http';
import { randomBytes, createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { exec } from 'node:child_process';

const env = Object.fromEntries(
  readFileSync(new URL('../../.env', import.meta.url), 'utf8')
    .split(/\r?\n/)
    .map((l) => l.match(/^\s*([A-Za-z_]\w*)\s*=\s*(.*)$/))
    .filter(Boolean)
    .map((m) => [m[1], m[2].trim().replace(/^["']|["']$/g, '')]),
);
const b64u = (b) => b.toString('base64url');
const disc = await (await fetch(env.OIDC_DISCOVERY_URL)).json();
const redirect = new URL(env.OIDC_REDIRECT_URI);
const verifier = b64u(randomBytes(32));
const state = b64u(randomBytes(16));
const nonce = b64u(randomBytes(16));
const auth = new URL(disc.authorization_endpoint);
auth.search = new URLSearchParams({
  response_type: 'code', client_id: env.OIDC_CLIENT_ID, redirect_uri: env.OIDC_REDIRECT_URI, scope: env.OIDC_SCOPE,
  state, nonce, code_challenge: b64u(createHash('sha256').update(verifier).digest()), code_challenge_method: 'S256',
}).toString();

const claims = (jwt) => (jwt ? JSON.parse(Buffer.from(jwt.split('.')[1], 'base64url').toString()) : null);
const show = (label, c) => {
  if (!c) return console.log(`${label}: (none)`);
  const { iss, aud, azp, typ, sub, exp, iat, scope, groups, realm_access, resource_access, preferred_username, email, upn, sid } = c;
  console.log(`${label}:`, JSON.stringify({ iss, aud, azp, typ, sub, lifetime_s: exp && iat ? exp - iat : undefined, scope,
    groups, realm_roles: realm_access?.roles, resource_access: resource_access && Object.keys(resource_access),
    preferred_username, email, upn, sid: sid ? 'present' : undefined, other_claims: Object.keys(c).sort() }, null, 1));
};
const token = async (form) => {
  const r = await fetch(disc.token_endpoint, { method: 'POST', body: new URLSearchParams(form) });
  return { status: r.status, body: await r.json() };
};

const server = createServer(async (req, res) => {
  const u = new URL(req.url, redirect.origin);
  if (u.pathname !== redirect.pathname) return res.writeHead(404).end();
  res.writeHead(200, { 'content-type': 'text/plain' }).end('Workbench probe: sign-in received, you can close this tab.');
  server.close();
  if (u.searchParams.get('state') !== state) return console.log('state mismatch');
  if (u.searchParams.get('error')) return console.log('error:', u.searchParams.get('error'), u.searchParams.get('error_description'));
  const t = await token({ grant_type: 'authorization_code', client_id: env.OIDC_CLIENT_ID, code: u.searchParams.get('code'),
    redirect_uri: env.OIDC_REDIRECT_URI, code_verifier: verifier });
  if (t.status !== 200) return console.log('code redemption failed', t.status, t.body.error, t.body.error_description);
  console.log('token response fields:', Object.keys(t.body), 'expires_in', t.body.expires_in, 'refresh_expires_in', t.body.refresh_expires_in);
  show('access token', claims(t.body.access_token));
  show('id token', claims(t.body.id_token));
  console.log('id token nonce matches:', claims(t.body.id_token)?.nonce === nonce);
  console.log('refresh token is a JWT:', (t.body.refresh_token?.split('.').length ?? 0) === 3);
  const r = await token({ grant_type: 'refresh_token', client_id: env.OIDC_CLIENT_ID, refresh_token: t.body.refresh_token });
  console.log('refresh:', r.status, r.body.error ?? 'ok', 'rotated refresh token:', r.status === 200 && r.body.refresh_token !== t.body.refresh_token);
  const ui = await fetch(disc.userinfo_endpoint, { headers: { authorization: `Bearer ${(r.body.access_token ?? t.body.access_token)}` } });
  console.log('userinfo:', ui.status, ui.ok ? Object.keys(await ui.json()) : '');
  // Leave no session behind.
  const rv = await fetch(disc.revocation_endpoint, { method: 'POST', body: new URLSearchParams({ client_id: env.OIDC_CLIENT_ID,
    token: r.body.refresh_token ?? t.body.refresh_token, token_type_hint: 'refresh_token' }) });
  console.log('revocation:', rv.status);
});
server.listen(Number(redirect.port), redirect.hostname === 'localhost' ? '127.0.0.1' : redirect.hostname, () => {
  console.log('Listening on', env.OIDC_REDIRECT_URI, '- opening the system browser for sign-in');
  exec(`start "" "${auth.toString()}"`);
});
setTimeout(() => { console.log('timed out waiting for sign-in'); process.exit(1); }, 300_000).unref();
