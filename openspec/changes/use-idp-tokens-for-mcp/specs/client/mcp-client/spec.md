# Spec Delta

## Purpose

The client presents the user's own identity-provider token to remote MCP servers.

## ADDED Requirements

### Requirement: Remote Server User Token
The client SHALL authenticate to each remote MCP server whose catalog entry has user-token authentication with the user's current access token from the organization identity provider, the token it also presents to the control plane. After a 401 from the server it SHALL refresh the token once and retry once. It MUST send no Authorization header to a server whose entry has authentication none, and MUST NOT send the token when a server redirects.

#### Scenario: User token presented
- **WHEN** the client calls a remote MCP server with user-token authentication
- **THEN** the bearer token presented is the user's current identity-provider access token

#### Scenario: Token refreshed after 401
- **WHEN** a remote MCP server answers 401
- **THEN** the client refreshes the access token and retries the request once with the new token

## REMOVED Requirements

### Requirement: Remote Server Authentication
**Reason**: The audience-scoped token came from the control plane's token exchange, which is removed (control-plane/identity Identity Provider Tokens Only).
**Migration**: Remote Server User Token.
