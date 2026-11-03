// Publishes a policy document to a Group on the local control plane, signed in as you (Authorization Code + PKCE in
// the system browser, as in oidc-probe.mjs). You must be a platform-admin there (dev-stack.ps1 -BootstrapAdmins).
// Token values are never printed or written; the refresh token is revoked at the end.
//   node dev/scripts/dev-policy.mjs <group> <policy.json> [control-plane-url]
//   node dev/scripts/dev-policy.mjs everyone dev/policies/everyone.json
import { createServer } from 'node:http';
import { randomBytes, createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { exec } from 'node:child_process';

const [group, file, cp = 'http://localhost:8080'] = process.argv.slice(2);
if (!group || !file) {
  console.log('usage: node dev/scripts/dev-policy.mjs <group> <policy.json> [control-plane-url]');
  process.exit(2);
}
const document = JSON.parse(readFileSync(file, 'utf8'));

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
const auth = new URL(disc.authorization_endpoint);
auth.search = new URLSearchParams({
  response_type: 'code', client_id: env.OIDC_CLIENT_ID, redirect_uri: env.OIDC_REDIRECT_URI, scope: env.OIDC_SCOPE,
  state, nonce: b64u(randomBytes(16)), code_challenge: b64u(createHash('sha256').update(verifier).digest()), code_challenge_method: 'S256',
}).toString();

async function publish(accessToken) {
  const call = async (method, path, body) => {
    const r = await fetch(cp + path, {
      method,
      headers: { authorization: `Bearer ${accessToken}`, 'content-type': 'application/json', accept: 'application/json' },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    const text = await r.text();
    if (!r.ok) throw new Error(`${method} ${path}: ${r.status} ${text.slice(0, 300)}`);
    return text ? JSON.parse(text) : {};
  };
  const base = `/api/v1/admin/policy/groups/${encodeURIComponent(group)}`;
  const known = await call('GET', '/api/v1/admin/policy/groups');
  const names = (Array.isArray(known) ? known : known.groups ?? []).map((g) => g.name);
  if (!names.includes(group)) await call('PUT', base, { description: 'set by dev-policy.mjs' });
  await call('PUT', `${base}/draft`, { document, priority: 0, reason: `dev-policy.mjs ${file}` });
  const p = await call('POST', `${base}/publish`, { reason: `dev-policy.mjs ${file}` });
  console.log(`published ${group}:`, JSON.stringify(p).slice(0, 300));
}

const server = createServer(async (req, res) => {
  const u = new URL(req.url, redirect.origin);
  if (u.pathname !== redirect.pathname) return res.writeHead(404).end();
  res.writeHead(200, { 'content-type': 'text/plain' }).end('Workbench dev-policy: sign-in received, you can close this tab.');
  server.close();
  let refresh;
  try {
    if (u.searchParams.get('state') !== state) throw new Error('state mismatch');
    if (u.searchParams.get('error')) throw new Error(`${u.searchParams.get('error')}: ${u.searchParams.get('error_description')}`);
    const r = await fetch(disc.token_endpoint, { method: 'POST', body: new URLSearchParams({ grant_type: 'authorization_code',
      client_id: env.OIDC_CLIENT_ID, code: u.searchParams.get('code'), redirect_uri: env.OIDC_REDIRECT_URI, code_verifier: verifier }) });
    const t = await r.json();
    if (r.status !== 200) throw new Error(`code redemption failed ${r.status} ${t.error}`);
    refresh = t.refresh_token;
    await publish(t.access_token);
  } catch (err) {
    console.log('failed:', err.message);
    process.exitCode = 1;
  } finally {
    if (refresh) await fetch(disc.revocation_endpoint, { method: 'POST', body: new URLSearchParams({ client_id: env.OIDC_CLIENT_ID,
      token: refresh, token_type_hint: 'refresh_token' }) });
    process.exit();
  }
});
server.listen(Number(redirect.port), redirect.hostname === 'localhost' ? '127.0.0.1' : redirect.hostname, () => {
  console.log('Opening the system browser for sign-in');
  exec(`start "" "${auth.toString()}"`);
});
setTimeout(() => { console.log('timed out waiting for sign-in'); process.exit(1); }, 300_000).unref();
