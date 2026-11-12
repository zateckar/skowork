# Design

## Context

- **Refresh lifetime today.** add-client-foundation D7 sets the refresh token to live 3600 s (`refresh_expires_in`) and to rotate on every refresh. A laptop that is asleep longer than an hour needs a new sign-in, and the browser's SSO session usually makes that prompt-free.
- **Sign-in required.** client/identity Sign-In Required State makes the client disconnected. Triggers and Remote tasks wait while it is in that state.
- **Scope.** The token manager sends the scope from machine configuration (`token_manager.cpp:65`). Nothing in the repository requests `offline_access`, and the refresh request sends no scope (`token_manager.cpp:371`).
- **Policy keys.** Each policy key names one owning capability, a default and a guardrail kind (`control-plane/src/policy/keys.ts`). A guardrail that sets a key to a stricter value applies to every user (control-plane/policy-service).

## Decisions

### D0. Reversal of add-client-foundation D7 (offline tokens)

The reversed decision is add-client-foundation D7, "Alternatives considered". It reads:

> Long-lived offline tokens (`offline_access`): they would outlive the SSO session and the client does not request them.

D7's risk list also says a stolen refresh token is "bound to the realm's SSO session limits" and that sign-out revokes it. The first part is no longer true for an offline token: it is bound to the offline session limits instead.

**Why it is reversed now.** The product owner asked to keep the user signed in as long as possible. The concern that offline tokens outlive the SSO session is accepted, and it is bounded in other ways: the realm's offline session idle and maximum limits (D1), revocation on sign-out (client/identity Sign-Out), the control plane's deny list (control-plane/identity), an organization switch that ends offline sessions (D5), and the existing encrypted store (client/local-storage). The one-hour refresh token was the cost of the old choice: every long sleep meant a browser sign-in.

When add-client-foundation is archived, its D7 keeps its text. This change records the reversal here and in its delta specs.

### D1. Realm prerequisites (operational, no code)

The realm owners must do the following. The client cannot do them, and this change does not change realm configuration. The user confirmed on 2026-11-10 that the realm already allows `offline_access`; the spike (task 1.1) still records the token type, the limits and rotation.

- Assign the optional client scope `offline_access` to the desktop client, and check that the user's realm roles allow it.
- Set the offline session idle limit and the maximum limit for offline sessions. These are the only time limits on the session. Record the chosen values in the realm runbook, not in this repository.
- Check the refresh token revocation setting. Rotation and reuse detection must apply to offline tokens as they do to normal refresh tokens, or D0's mitigation for theft does not hold.

**Spike.** Task 1.1 confirms the realm behaviour with the probe script before any code is written. The spike checks the token type, the lifetime fields, what the realm does when the scope is not assigned, and whether rotation applies.

**Alternatives considered**
- *Assume the realm is configured:* a silent fallback hides a missing scope from users. It is kept as a fallback (D3), but the spike records the real behaviour.
- *Request a longer normal refresh token lifetime instead:* this lengthens a token that is still bound to the SSO session and the same one-hour rotation cycle. It does not give the same session length, and it changes the realm for every client.

### D2. The client requests offline_access only while policy allows it

When policy.identity.offline_sign_in is on, the client adds `offline_access` to the scopes of the authorization request. When it is off, the scope is not added (client/identity Offline Sign-In Request).

**Alternatives considered**
- *Always request it:* removes the organization's ability to turn the feature off.
- *A machine configuration flag:* a configuration change needs the installer, and the user would have no per-Group control.
- *Per-user opt-in:* users would not know the trade-off, and the default would leave most sessions short.

### D3. Token type check and fallback

After the token response, the client reads the type of the refresh token. An offline token is accepted as an offline token. When the realm did not issue one, the client keeps the normal refresh token, signs the user in, and reports "Offline sign-in not available". The sign-in does not fail, because the user can still work for an hour, as today.

**Alternatives considered**
- *Fail the sign-in when no offline token is issued:* a realm misconfiguration would then lock every user out.
- *Assume the token type from the policy:* the policy says what was asked, not what was granted.

### D4. No client-side lifetime

The client sets no session lifetime of its own. The session ends when the identity provider refuses a refresh (`invalid_grant`), on sign-out, or when D5 turns the feature off. Each of these enters the sign-in required state. This keeps the session as long as the realm allows, as the user asked.

**Alternatives considered**
- *A client cap, for example 30 days without use:* stricter, and easy to add later as a policy key. It was not asked for and would push users back to the browser. It is an open question below.

### D5. Policy switch ends offline sessions

policy.identity.offline_sign_in has values on | off, the default is on, and its guardrail kind is `max-level` with the values ordered `off`, `on`: an organization guardrail set to off applies to every user, and Group policies cannot turn it back on. The guardrail mode `stricter` (control-plane/policy-service) applies only to decision maps, so an on/off switch uses `max-level` as `apps.mode` does. When the effective policy becomes off, the client revokes its stored offline token at the identity provider at the next policy update, deletes it and enters the sign-in required state. It does not open the browser.

Revoking the token is needed. Only stopping new offline requests would leave a long-lived token working until the realm ends it.

**Alternatives considered**
- *Let the existing offline session run until the realm ends it:* the organization's switch would then not stop a session that is already active, which makes the switch unreliable for incident response.
- *Delete the token without revoking it:* the token would stay valid at the identity provider. Revocation is required.

**Implementation details (added while implementing task 2.x).**
- The client reads the token type from the refresh token's `typ` claim (`Offline` or `Refresh`; Keycloak sets it). The type is read from the stored token, not kept in a setting, so a restart shows the same status (client/identity Offline Token Type).
- The policy reaches the token manager through `TokenManager::set_offline_sign_in`. The app calls it on each accepted bundle, and the cached bundle is loaded before the token manager starts, so an off policy is enforced before the first refresh of a stored offline token.
- If the revocation request fails, the token is still deleted and the client records the security event `offline_revoke_failed`. The token then stays valid at the identity provider until the realm ends it, the same gap as in D7. The event lets the administrator see it.

**Owner.** client/identity owns the key. control-plane/policy-service stores it and applies the guardrail. Both specs are in flight. The key needs the top-level `identity` entry in the policy bundle key list (see Glossary below).

### D6. Control plane accepts offline-session access tokens unchanged

The control plane does not change its trust rules. An access token from an offline session is an access token: it is 5 minutes long, signed by the realm, and checked for issuer, audience, authorized party and lifetime, exactly as today (control-plane/identity Identity Provider Trust and Short Access Token Lifetime). The control plane never requests, stores or uses offline or refresh tokens, and it does not use the `sid` claim to decide trust. The deny list (control-plane/identity Deny List) still refuses installations and users with 403.

Checked and not affected:
- The fleet registry's checks: it checks the principal, Groups, roles and the token lifetime, not the token's session. No change to control-plane/fleet-registry.
- The token exchange in accepted control-plane/identity: an exchanged token expires no later than the presented token, which is still 5 minutes. It is also removed by use-idp-tokens-for-mcp.
- Remote MCP servers (use-idp-tokens-for-mcp): they get the user's current access token. The client refreshes it the same way as for the control plane, so no change.

**Alternatives considered**
- *Track offline sessions in the control plane through `sid` and revoke them at the identity provider:* the control plane would need an identity-provider administrator credential and a session store. This is a larger decision, and it is an open question below.

### D7. Revocation paths

| Event | Effect |
|---|---|
| The user chooses Sign out | The client revokes its offline token and deletes it (client/identity Sign-Out). |
| The organization turns the policy off | D5. |
| An administrator revokes an installation or user in the control plane | The deny list refuses its requests with 403 (control-plane/identity). The client then revokes and deletes its offline token (see below). A copy of the token stays valid at the identity provider until the realm session is ended. |
| An administrator disables the account in the realm | The realm refuses refresh, so the client enters the sign-in required state (existing control-plane/identity rule). |

The second-to-last row is the gap. An installation revoked in the control plane can still obtain access tokens, and those tokens still work at remote MCP servers that accept them.

**Client-side closure (2026-11-12).** When registration answers that the installation or user is revoked, the client itself revokes its offline token at the identity provider, deletes it and enters the sign-in required state (client/identity Offline Session Ends On Revoked Access). This needs no identity-provider administrator credential in the control plane, so decided question 1 stands. It covers every client that is running or starts again. It does not cover a token copied out of the profile, or a client that never contacts the control plane again: for those the realm administrator still ends the offline session or disables the account.

- *Alternative: the control plane revokes at the identity provider.* Rejected in decided question 1.
- *Alternative: also end a normal refresh token.* Not needed: it ends with the browser SSO session within the hour-scale realm limits, and keeping it lets the client re-register when access is restored.

### D8. Unattended runs

Offline sessions let Triggers and Remote tasks run for as long as the realm session allows, even when the user has not signed in for weeks. This change does not add a separate limit. The existing policy.unattended keys still apply to what a run may do. The question of a limit on unattended runs is open.

## Spike

To be recorded after task 1.1.

## Glossary and roadmap (owned by the product owner; not edited here)

- **Policy bundle top-level keys:** add `identity`, which holds policy.identity.offline_sign_in.
- **New term, proposed:** "Offline sign-in: the client's sign-in that keeps a refresh token valid beyond the browser SSO session, under the realm's offline session limits (client/identity)."
- **Roadmap:** 42 add-offline-sign-in [Hardening] is already listed.

## Decided questions (user, 2026-11-10)

1. The control plane does not revoke the realm's offline session when an installation is revoked. The realm administrator does it by hand (D7). Automating it is out of scope.
2. No client-side cap on session lifetime (D4).
3. Unattended runs need no recent interactive sign-in (D8).
4. The realm owners choose the offline session idle and maximum values and record them in the realm runbook (D1).

## Risks / Trade-offs

- [A stolen profile contains a long-lived offline token] -> The token is encrypted under the DPAPI-protected database key and the profile is readable only by the user (client/local-storage). The realm's idle and maximum limits end it. Sign-out and D5 revoke it.
- [A revoked installation keeps an offline token] -> The control plane refuses its requests with 403, and the client then revokes and deletes its own offline token (D7). For a copied token or a client that stays away, the realm administrator ends the session or disables the account.
- [Realm not configured for offline_access] -> The fallback in D3 keeps the one-hour behaviour and shows the status. The spike (task 1.1) confirms the realm behaviour first.
- [Unattended runs continue for weeks without a user] -> The existing policy.unattended keys apply. Open question 3 decides whether a further limit is needed.
- [A copied offline token is used quietly] -> Rotation and reuse detection in the realm (D1) end the session when the original and the copy both refresh.
- [A policy switch reaches the client late] -> The client enforces D5 at the next policy update. Until then it keeps the last accepted bundle, as client/policy-enforcement already requires while offline.

## Acceptance

To be recorded after task 4.2.
