# Proposal

## Why

On 2026-11-05 the product owner reported that a remote MCP server could not be enabled: discovery failed with "the server answered HTTP 401". The server trusts only the organization identity provider (Keycloak). The control plane, however, presented a token it had signed itself (add-core-tools-and-mcp D6): during discovery a service token for the subject skowork-catalog-discovery, and on clients a token from its own token exchange.

Organization rule (stated 2026-11-05): every application trusts only the central identity provider. No application issues identities to other applications. Control-plane-signed tokens therefore cannot be accepted by MCP servers, now or later.

## What Changes

- **Client.** A remote MCP server with user-token authentication receives the user's own identity-provider access token, the same token the client presents to the control plane. It is refreshed the same way, and refreshed once after a 401.
- **Discovery.** The catalog connects to a remote server with the identity-provider access token of the administrator whose request started the discovery (registration, update, re-discovery). The catalog never discovers in the background, so it never needs a token of its own.
- **Catalog entry.** The authentication mode audience-token becomes user-token, and an entry no longer has an audience. Stored entries are migrated.
- **Removed:** the control plane's token exchange, its signing keys for exchanged tokens, the well-known endpoint publishing them and the key rotation endpoint. The control plane issues no tokens.

Out of scope:
- Token exchange at the identity provider (RFC 8693) to narrow the audience per server. It needs identity-provider configuration per MCP server; it can be added later as another authentication mode.
- Authentication schemes other than bearer tokens (API keys, mutual TLS).
- MCP servers the user adds outside the catalog.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `control-plane/identity`: Audience-Scoped Token Exchange (removed), Exchanged Token Signing Key Publication (removed), Identity Provider Tokens Only (added).
- `control-plane/mcp-catalog`: Catalog Entry Rules, Entry Delivered In The Policy Bundle and Tool Discovery Snapshot (modified); Remote Audience Authorization (removed).
- `client/mcp-client`: Remote Server Authentication (removed), Remote Server User Token (added).

## Impact

- **Roadmap:** 32, after keep-sign-in-response-on-root. Depends on add-core-tools-and-mcp (archived) and add-client-foundation (Identity Provider Trust, the client's token handling).
- **Code:**
  - control-plane/src/identity: the token exchange and its routes are removed;
  - control-plane/src/mcp: the auth mode, the discovery token, a migration of stored entries;
  - control-plane/admin: the authentication choice of a remote entry;
  - client/src/mcp and client/src/app: the token source of remote servers.
- **Tests:** control-plane MCP tests (discovery presents the administrator's token, no exchange endpoint), client MCP tests (the user token is presented and refreshed after a 401), the MCP test server checks the issuer instead of a per-server audience.
- **Data:** a migration rewrites stored entries from audience-token to user-token and deletes the exchanged-token signing keys.
- **Operations:** a remote MCP server must accept the identity provider's access tokens of the Skowork client, which the desktop client and the admin console share; see design.md D3.
