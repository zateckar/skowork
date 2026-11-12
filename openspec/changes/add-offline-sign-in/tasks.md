# Tasks

## 1. Validation spike

- [ ] 1.1 Probe the organization test realm with dev/scripts/oidc-probe.mjs (one interactive sign-in, only after the user's OK): an offline token is issued with offline_access; its type and lifetime fields; the result when the scope is not assigned to the client; rotation and reuse on refresh; the realm's offline idle and maximum values, which the realm owners confirm. Verify: results and the realm values are recorded in design.md Spike and D1
- [ ] 1.2 Probe revocation: revoke an offline token at the revocation endpoint and refresh with it. Verify: the refresh is refused with invalid_grant, and the result is recorded in design.md Spike

## 2. Client sign-in and token manager

- [x] 2.1 Read policy.identity.offline_sign_in (default on) and add offline_access to the sign-in request only while it is on (D2). Verify with unit tests against a fake identity provider that inspect the request body: on adds the scope, off omits it, and a policy change applies to the next sign-in
- [x] 2.2 Check the type of the received refresh token and fall back to the normal token with the "Offline sign-in not available" status (D3). Verify with unit tests: an offline token sets the status to active; a normal token keeps the sign-in and shows the status
- [x] 2.3 Revoke, delete and enter the sign-in required state when the effective policy turns the feature off, without opening the browser (D5). Verify with unit tests: revocation request sent, token deleted, no browser opened, and the next sign-in omits the scope
- [x] 2.4 Keep an offline session across a long absence and end it on invalid_grant (D4). Verify with unit tests using the fake identity provider and a fake clock: a stored offline token refreshes after 20 days without the browser; invalid_grant deletes it and enters the sign-in required state
- [x] 2.6 End an offline session when the control plane reports the installation or user as revoked (D7, Offline Session Ends On Revoked Access). Verify with a unit test: an offline token is revoked and deleted and the state is sign-in required; a normal token is kept and no revocation request is sent

- [x] 2.5 Confirm that sign-out revokes and deletes an offline token (existing Sign-Out requirement). Verify with a unit test that inspects the revocation request

## 3. Control plane

- [x] 3.1 Accept access tokens from offline sessions under the existing rules (D6). Verify with tests in control-plane/test/identity.test.ts using locally signed tokens: a fresh 5-minute token is accepted without a sid check; a token valid for 60 minutes is still refused; a deny-listed installation gets 403
- [x] 3.2 Register policy.identity.offline_sign_in in control-plane/src/policy/keys.ts with owner client/identity, default on and guardrail stricter (D5). Verify with a test that a guardrail set to off wins over a Group set to on, and that the key is accepted in a policy document

## 4. Acceptance (needs the user's OK; real identity-provider sign-in)

- [ ] 4.1 On a built client against the test realm, sign in once, stop the client for at least 65 minutes, start it again and check that no browser opens and the client reaches the signed-in state. Verify: result recorded in design.md Acceptance
- [ ] 4.2 Set policy.identity.offline_sign_in to off for the test user's Group, wait for the next policy update, and check the client shows the sign-in required state and that the old offline token is refused at the realm. Verify: result recorded in design.md Acceptance

## 5. Validation

- [ ] 5.1 Run `npm --prefix control-plane test` and the client tests (`cmake --build --preset test` in client/). Verify: all pass. Status: partial, 2026-11-12: the client test tree builds and the TokenManagerOfflineSignIn tests pass; the full client and control-plane suites were not rerun
- [x] 5.2 Run `openspec validate --all --strict`. Verify: it passes with no warnings
