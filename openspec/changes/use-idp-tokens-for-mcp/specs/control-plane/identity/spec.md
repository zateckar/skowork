# Spec Delta

## Purpose

Removes the control plane's own tokens for MCP servers. Every token a Skowork component presents to another service is issued by the organization identity provider.

## ADDED Requirements

### Requirement: Identity Provider Tokens Only
The control plane SHALL NOT issue access tokens or identity assertions for other services. When a Skowork component authenticates to another service on behalf of a user, it MUST present that user's access token from the organization identity provider, accepted under Identity Provider Trust.

#### Scenario: No token exchange
- **WHEN** a client asks the control plane for a token for another service
- **THEN** there is no such endpoint, and no control-plane signing key for tokens is published

## REMOVED Requirements

### Requirement: Audience-Scoped Token Exchange
**Reason**: The organization trusts only the central identity provider; a control-plane-signed token is not accepted by other services.
**Migration**: Clients present the user's identity-provider access token to remote MCP servers (client/mcp-client Remote Server Authentication).

### Requirement: Exchanged Token Signing Key Publication
**Reason**: The control plane no longer signs tokens, so there are no keys to publish.
**Migration**: MCP servers verify tokens against the identity provider's published keys.
