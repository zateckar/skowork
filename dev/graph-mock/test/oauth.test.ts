// dev/graph-mock Mock OAuth Endpoints, Mock OAuth Fault Injection, Mock Authentication (tasks 2.1, 2.1a).
import { test, after, before } from 'node:test';
import assert from 'node:assert/strict';
import { decodeJwt } from 'jose';
import type { GraphMock } from '../src/mock.ts';
import { startMock, signIn, refresh, graph, control, ALL_SCOPES } from './helpers.ts';

let m: GraphMock;
before(async () => { m = await startMock(); });
after(async () => { await m.close(); });

test('discovery names the mock as issuer and its endpoints', async () => {
  const r = await fetch(`${m.graphBase}/mock-tenant/v2.0/.well-known/openid-configuration`);
  const d = (await r.json()) as Record<string, string>;
  assert.equal(d.issuer, `${m.graphBase}/mock-tenant/v2.0`);
  assert.equal(d.authorization_endpoint, `${m.graphBase}/mock-tenant/oauth2/v2.0/authorize`);
  assert.equal(d.token_endpoint, `${m.graphBase}/mock-tenant/oauth2/v2.0/token`);
});

test('a full code + PKCE flow gives access and refresh tokens, and refresh gives a new access token', async () => {
  const s = await signIn(m, { user: 'eva' });
  assert.equal(s.status, 200, JSON.stringify(s.body));
  assert.equal(s.redirect?.searchParams.get('state'), 's1');
  const t = s.tokens!;
  assert.ok(t.refresh_token);
  const claims = decodeJwt(t.access_token);
  assert.equal(claims.upn, 'eva.schneider@acme-test.example');
  assert.equal(claims.iss, `${m.graphBase}/mock-tenant/v2.0`);
  assert.deepEqual(String(claims.scp).split(' ').sort(), ['Calendars.ReadWrite', 'Mail.ReadWrite', 'Mail.Send', 'MailboxSettings.Read', 'User.Read']);
  const me = await graph(m, t.access_token).get('/me');
  assert.equal(me.status, 200);
  assert.equal(me.json.userPrincipalName, 'eva.schneider@acme-test.example');

  const r = await refresh(m, t.refresh_token!);
  assert.equal(r.status, 200);
  assert.notEqual(r.body.access_token, t.access_token);
  assert.notEqual(r.body.refresh_token, t.refresh_token);
  // The old refresh token was rotated out.
  assert.equal((await refresh(m, t.refresh_token!)).body.error, 'invalid_grant');
});

test('the default user signs in without a login hint', async () => {
  const s = await signIn(m);
  assert.equal(decodeJwt(s.tokens!.access_token).upn, 'jan.novak@acme-test.example');
});

test('a wrong verifier gets invalid_grant, and a code is single use', async () => {
  const s = await signIn(m, { verifier: 'not-the-verifier-not-the-verifier-not-the-verifier-0000' });
  assert.equal(s.status, 400);
  assert.equal(s.body.error, 'invalid_grant');
});

test('authorize refuses non-loopback redirects and requires S256', async () => {
  const bad = await fetch(`${m.graphBase}/mock-tenant/oauth2/v2.0/authorize?client_id=x&response_type=code&redirect_uri=https%3A%2F%2Fevil.example%2F&code_challenge=c&code_challenge_method=S256`, { redirect: 'manual' });
  assert.equal(bad.status, 400);
  const plain = await fetch(`${m.graphBase}/mock-tenant/oauth2/v2.0/authorize?client_id=x&response_type=code&redirect_uri=http%3A%2F%2Flocalhost%3A1%2F&code_challenge=c&code_challenge_method=plain`, { redirect: 'manual' });
  assert.equal(new URL(plain.headers.get('location')!).searchParams.get('error'), 'invalid_request');
});

test('requests without a mock token are rejected like Graph does', async () => {
  const none = await fetch(`${m.graphBase}/v1.0/me`);
  assert.equal(none.status, 401);
  assert.equal(((await none.json()) as any).error.code, 'InvalidAuthenticationToken');
  const forged = await graph(m, 'eyJhbGciOiJub25lIn0.eyJ1cG4iOiJqYW4ifQ.').get('/me');
  assert.equal(forged.status, 401);
});

test('a token without Mail.Send cannot send', async () => {
  const s = await signIn(m, { scope: 'https://graph.microsoft.com/Mail.ReadWrite offline_access' });
  const g = graph(m, s.tokens!.access_token);
  assert.equal((await g.get('/me/messages?$top=1')).status, 200);
  const send = await g.post('/me/sendMail', { message: { subject: 'x', toRecipients: [{ emailAddress: { address: 'a@example.com' } }] } });
  assert.equal(send.status, 403);
  assert.equal(send.json.error.code, 'ErrorAccessDenied');
  assert.equal((await g.get('/me/events/x')).status, 403);
});

test('injected OAuth faults are returned exactly as configured', async () => {
  const c = control(m);
  // interaction_required and consent_required at the authorize step.
  await c.post('/oauth/faults', { kind: 'interaction_required', step: 'authorize', count: 1 });
  let s = await signIn(m);
  assert.equal(s.body.error, 'interaction_required');
  assert.equal(s.redirect?.searchParams.get('state'), 's1');
  await c.post('/oauth/faults', { kind: 'consent_required', step: 'authorize', user: 'tom', count: 1 });
  s = await signIn(m, { user: 'eva' });
  assert.ok(s.tokens, 'the fault was for another user');
  s = await signIn(m, { user: 'tom' });
  assert.equal(s.body.error, 'consent_required');

  // At the token step of a refresh: interaction_required, then invalid_grant (which also revokes the token).
  const ok = await signIn(m);
  await c.post('/oauth/faults', { kind: 'interaction_required', step: 'refresh', count: 1 });
  let r = await refresh(m, ok.tokens!.refresh_token!);
  assert.equal(r.status, 400);
  assert.equal(r.body.error, 'interaction_required');
  r = await refresh(m, ok.tokens!.refresh_token!);
  assert.equal(r.status, 200, 'one use only');
  await c.post('/oauth/faults', { kind: 'invalid_grant', step: 'refresh', count: 1 });
  r = await refresh(m, String(r.body.refresh_token));
  assert.equal(r.body.error, 'invalid_grant');

  // An expired access token: Graph answers 401 so that the client refreshes.
  await c.post('/oauth/faults', { kind: 'expired_token', count: 1 });
  const exp = await signIn(m);
  const me = await graph(m, exp.tokens!.access_token).get('/me');
  assert.equal(me.status, 401);
  assert.match(me.json.error.message, /expired/);
  assert.equal((await refresh(m, exp.tokens!.refresh_token!)).status, 200);
});

test('token lifetimes are configurable and refresh tokens can be revoked', async () => {
  const c = control(m);
  await c.post('/oauth/config', { access_token_lifetime_s: 120 });
  const s = await signIn(m, { scope: ALL_SCOPES });
  assert.equal(s.tokens!.expires_in, 120);
  await c.post('/oauth/config', { access_token_lifetime_s: 3600 });
  await c.post('/oauth/revoke', { user: 'jan' });
  assert.equal((await refresh(m, s.tokens!.refresh_token!)).body.error, 'invalid_grant');
});
