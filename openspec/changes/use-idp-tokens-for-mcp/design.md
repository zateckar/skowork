# Design

## Context

add-core-tools-and-mcp D6 made the control plane a token issuer for MCP servers. Clients exchanged their Keycloak access token at `POST /api/v1/identity/token-exchange` for an ES256 token signed with the control plane's `mcp-token` key ring, with the server as the only audience. Catalog discovery signed a 60-second token for the subject `skowork-catalog-discovery`. MCP servers had to trust the control plane's keys at `/.well-known/skowork-token-keys.json`.

The organization does not allow this. Every application trusts only the central identity provider, and no application vouches for identities towards another. An MCP server that validates against Keycloak answers 401 to a control-plane token. This is how the failure was found (2026-11-05).

## Goals / Non-Goals

- **Goals:**
  - every bearer token sent to an MCP server comes from Keycloak;
  - the control plane issues no tokens and holds no token-signing keys;
  - discovery works against a server that accepts only Keycloak tokens.
- **Non-Goals:**
  - narrowing the audience per server (RFC 8693 exchange at Keycloak);
  - other authentication schemes.

## Decisions

### D1. Clients present the user's access token
- **Choice:** For an entry with `auth: user-token` the client sends `Authorization: Bearer <access token>`, with the same token the control-plane client sends. The token comes from the client's token manager, which refreshes it when less than 60 seconds remain. When the server answers 401, the client calls the token manager's refresh-after-unauthorized with its current token and retries once. That 401 never moves the client to sign-in required, because only the control plane's rejection does that. Redirects are not followed (unchanged), so the token never reaches another host.
- **Why:** this is the organization rule, and it is the smallest change. The token is already in memory and refreshed.
- **Alternatives considered:**
  - Token exchange at Keycloak (RFC 8693, per-server audience). This keeps the token narrow, but it needs a Keycloak client and exchange permission for every MCP server, and the product owner asked for the user's own token. It stays possible later as another mode.
  - Keep the control-plane exchange and ask MCP servers to trust it. The organization rule forbids it.

### D2. Discovery uses the administrator's token
- **Choice:** Discovery runs only inside an administrator's request: create, update and `POST servers/{id}/discover`. The route passes the request's bearer token, the administrator's Keycloak token already accepted by Identity Provider Trust, to the catalog, which presents it to the server. There is no background re-discovery, so the catalog never needs a token of its own.
- **Why:** an administrator who registers a server is a real user the server can authorize. A service identity would need Keycloak client credentials that the control plane does not have.
- **Consequence:** the admin console and the desktop client sign in through the same Keycloak client, so the server sees the same authorized party in both cases.
- **Alternatives considered:**
  - A Keycloak client-credentials token for the control plane. This needs a client secret in the control plane, and MCP servers would see a service account instead of a person.
  - Discovery from a client. The catalog's review flow (Tool Discovery Snapshot) would then depend on a user's machine.

### D3. What an MCP server must accept
- **Choice:** An MCP server verifies the signature against the Keycloak realm's JWKS, the issuer of the realm, expiry, and the authorized party (`azp`) of the Skowork client. Keycloak puts `aud` only where an audience mapper adds it. A server that checks `aud` needs an audience mapper on the Skowork client that adds the server's audience. This is an identity-provider setting, not catalog data. It is documented in the deployment guidance (README, Remote MCP servers), not in the catalog.
- **Why:** the catalog cannot make Keycloak add an audience, so it no longer keeps one.

### D4. Catalog data
- **Choice:** `auth` takes `user-token` (the default for remote entries) or `none`. `audience` is dropped. A migration rewrites the `entry` JSON of `mcp_servers`: `audience-token` becomes `user-token` and `audience` is removed. It also deletes the rows of the `mcp-token` key ring. Past version snapshots stay as they were recorded. `SKOWORK_MCP_AUDIENCES` (`identity.mcpAudiences`) is removed.
- **Why:** a mode named after a token that no longer exists would mislead administrators.
- **Alternatives considered:**
  - Keep the name audience-token. It would be wrong.
  - Accept both names. That would be a compatibility shim with no client that needs it, because clients read the Policy bundle, which is rebuilt from the migrated entries.

### D5. Removed code
- **Choice:** `TokenExchange` (`control-plane/src/identity/tokens.ts`) is removed, together with `POST /api/v1/identity/token-exchange`, `/.well-known/skowork-token-keys.json` and `POST /api/v1/admin/identity/token-keys/rotate`. The client's `exchange_token` and its token cache per audience are removed too.
- **Why:** nothing uses them, and keeping a token issuer around invites its use.

## Risks / Trade-offs

- [Risk] The user's token is valid for the control plane and other services that accept the Skowork client, so a compromised MCP server could replay it within its 5-minute lifetime. -> Mitigation: tokens go only to catalog entries that a platform-admin registered and assigned, over HTTPS, never to a host they redirect to. The organization accepts this as the consequence of trusting only the identity provider. Per-server audiences (D1, alternative 1) are the follow-up if it is not acceptable.
- [Risk] A server that checks `aud` still refuses the token. -> Mitigation: D3 documents the audience mapper. The discovery error names the HTTP status, so the administrator sees it at registration.
- [Risk] An administrator's token expires during a long discovery. -> Mitigation: discovery has a timeout (`mcpCatalog.discoveryTimeoutMs`) far below the 5-minute token lifetime, and the admin console refreshes tokens before each call.

## Migration Plan

1. Deploy the control plane. The migration rewrites the entries, and the Policy bundles carry `user-token`.
2. Clients that receive the new bundle send the user's token. An older client does not know `user-token` and leaves the server unavailable until it is updated. There are no production users yet.
3. Re-run discovery for entries that failed with 401.
