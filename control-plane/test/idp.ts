// The in-test identity provider of the admin console end-to-end tests and the console screenshot tool.
import assert from 'node:assert/strict';
import { createHash, randomUUID } from 'node:crypto';
import { createServer, type Server } from 'node:http';
import { json, type Harness } from './helpers.ts';

/** An identity provider for the browser: /authorize signs in `next.user` at once; /token issues the harness's tokens. */
export async function fakeIdp(h: Harness) {
  const next = { user: 'admin1' };
  const codes = new Map<string, { user: string; challenge: string }>();
  const server: Server = createServer((req, res) => {
    const url = new URL(req.url ?? '/', 'http://localhost');
    const base = `http://127.0.0.1:${(server.address() as { port: number }).port}`;
    // The console fetches discovery and tokens from its own origin, as it does from Keycloak.
    res.setHeader('Access-Control-Allow-Origin', '*');
    if (url.pathname === '/.well-known/openid-configuration') {
      return json(res, 200, { issuer: base, authorization_endpoint: `${base}/authorize`, token_endpoint: `${base}/token`, jwks_uri: `${base}/jwks` });
    }
    if (url.pathname === '/authorize') {
      assert.equal(url.searchParams.get('code_challenge_method'), 'S256');
      const code = randomUUID();
      codes.set(code, { user: next.user, challenge: url.searchParams.get('code_challenge')! });
      const back = new URL(url.searchParams.get('redirect_uri')!);
      back.search = new URLSearchParams({ code, state: url.searchParams.get('state')! }).toString();
      res.writeHead(302, { location: back.toString() }).end();
      return;
    }
    if (url.pathname === '/token' && req.method === 'POST') {
      let raw = '';
      req.on('data', (c) => (raw += c));
      req.on('end', async () => {
        const f = new URLSearchParams(raw);
        let user: string | undefined;
        if (f.get('grant_type') === 'authorization_code') {
          const c = codes.get(f.get('code') ?? '');
          codes.delete(f.get('code') ?? '');
          // PKCE: the verifier must hash to the challenge.
          const hashed = createHash('sha256').update(f.get('code_verifier') ?? '').digest('base64url');
          if (c && hashed === c.challenge) user = c.user;
        } else if (f.get('grant_type') === 'refresh_token') {
          user = f.get('refresh_token') ?? undefined;
        }
        if (!user) return json(res, 400, { error: 'invalid_grant' });
        json(res, 200, { access_token: await h.token(user), token_type: 'Bearer', expires_in: 300, refresh_token: user });
      });
      return;
    }
    json(res, 404, {});
  });
  await new Promise<void>((r) => server.listen(0, '127.0.0.1', () => r()));
  const port = (server.address() as { port: number }).port;
  return { next, url: `http://127.0.0.1:${port}/.well-known/openid-configuration`, close: () => new Promise<void>((r) => (server.closeAllConnections(), server.close(() => r()))) };
}
