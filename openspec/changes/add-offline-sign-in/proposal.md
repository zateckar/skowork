# Proposal

## Why

The client's refresh token lives one hour and rotates on every use (add-client-foundation D7). A laptop that sleeps longer than that must sign the user in again. A client in the sign-in required state counts as disconnected (client/identity Sign-In Required State), so it stops receiving pushes and cannot start Runs that wait for the connection. The product owner asked to keep the user signed in as long as possible.

Keycloak can issue offline tokens (the `offline_access` scope). They let the client refresh for as long as the realm's offline session limits allow. add-client-foundation D7 rejected them. This change reverses that decision; design.md D0 records why.

## What Changes

- **Offline sign-in.** When policy.identity.offline_sign_in is on, the client requests `offline_access` at sign-in. The stored refresh token is then an offline token, and refreshes keep working across sleeps and across the browser's SSO session limit.
- **Fallback.** If the realm does not issue an offline token, the client keeps the normal refresh token, signs the user in, and shows "Offline sign-in not available".
- **Policy switch.** policy.identity.offline_sign_in (on or off, default on) is an ordinary policy key. An organization guardrail can set it to off. When the effective policy turns it off, the client revokes its stored offline token at the next policy update and enters the sign-in required state.
- **Control plane.** Access tokens from offline sessions are accepted under the same rules as any other access token. The control plane never requests, stores or uses offline or refresh tokens.
- **Realm prerequisite (operational).** The realm owners assign `offline_access` to the desktop client and set the offline session idle and maximum limits. This change does not change realm configuration.

Out of scope:
- The control plane revoking a Keycloak session on its own when an installation or user is revoked. It would need an identity-provider administrator credential. See design.md Open questions.
- A client-side cap on session lifetime. The realm limits apply.
- Limiting unattended runs by the time since the last interactive sign-in. See design.md Open questions.
- Remote MCP server token handling (use-idp-tokens-for-mcp), the access token lifetime and the token exchange.
- Any change to the sign-in flow itself (PKCE, loopback redirect, ID token checks).

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/identity` (added by add-client-foundation, not yet accepted): Identity Provider Configuration and Refresh Token Protection And Rotation are modified. Offline Sign-In Request, Offline Session Lifetime and Offline Sign-In Policy are added.
- `control-plane/identity`: Offline Session Tokens is added.

Checked and left unchanged: client/local-storage (it stores the token but does not describe token types), client/browser-use (the agent browser keeps its own identity-provider session), control-plane/fleet-registry (token lifetime checks are unchanged; see design.md D7).

## Impact

- **Roadmap:** 42 [Hardening]. It depends on add-client-foundation (1), which adds client/identity and the control-plane/identity delta this change modifies, and on add-policy-and-sandbox (3), which defines the policy bundle and guardrails. It is archived after both.
- **Code:**
  - client/src/identity/oidc.cpp: the authorize request scope.
  - client/src/identity/token_manager.cpp: token type check, fallback, the policy-off revocation and deletion.
  - control-plane/src/policy/keys.ts: the new policy key, owner client/identity.
  - control-plane/test/identity.test.ts: offline-session token tests.
- **Data:** no new columns. The client's stored refresh token may be an offline token in the same secret slot.
- **Glossary and roadmap (owned by the product owner; not edited here):** the Policy bundle top-level key list needs `identity`, and the term "Offline sign-in" is proposed. See design.md.
- **Keycloak realm (operational prerequisite):** `offline_access` assigned to the desktop client; offline session idle and maximum limits set; refresh token revocation settings checked. See design.md D1.
