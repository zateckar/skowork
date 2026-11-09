# control-plane/identity Specification

## Purpose
Defines how the control plane trusts access tokens issued by the organization identity provider (Keycloak), assigns the caller's Groups and admin roles, and exchanges a user's token for audience-scoped tokens for remote MCP servers. The identity provider's tokens remain the only user credential, and Groups come from control-plane/identity, not from a token claim.

## Requirements

### Requirement: Audience-Scoped Token Exchange
The control plane SHALL exchange a valid user access token, accepted under Identity Provider Trust, for a token scoped to a single remote MCP server registered in control-plane/mcp-catalog, when the user's Groups permit that server. The exchanged token is signed by the control plane, carries the same subject, and is valid for no longer than the presented token. The exchange is refused otherwise. Only user access tokens from allowed clients can be exchanged.

#### Scenario: Token for a remote MCP server
- **WHEN** a client requests a token for an MCP server allowed for the user's Groups
- **THEN** it receives a token whose audience is that server and whose expiry is no later than the presented token's

#### Scenario: Audience not permitted
- **WHEN** a client requests a token for an MCP server not allowed for the user's Groups
- **THEN** the exchange is refused and an audit event is recorded

#### Scenario: Token from a client that is not allowed
- **WHEN** a client presents a token whose authorized party is not an allowed client and requests an exchange
- **THEN** the exchange is refused with 401 and an audit event is recorded

### Requirement: Exchanged Token Signing Key Publication
The control plane SHALL sign exchanged tokens with an asymmetric key and publish the current and next public keys at a well-known endpoint. Keys rotate without invalidating unexpired exchanged tokens. The keys for exchanged tokens are separate from the policy signing keys of control-plane/policy-service.

#### Scenario: Key rotation
- **WHEN** the exchanged-token signing key is rotated
- **THEN** tokens signed with the previous key stay valid until they expire, and new tokens use the new key

### Requirement: Admin Directory
control-plane/identity SHALL list, for any holder of an administrator role, the user names and identity-provider roles it knows, for choosing them in the admin console. The list holds names only, never token contents or telemetry.

#### Scenario: Directory for a picker
- **WHEN** alice registered an installation, bob was assigned to the Group sales, and a platform-admin requests the directory
- **THEN** the users include alice and bob, and the bootstrap administrators are marked as such

### Requirement: Known Users And Roles
A user SHALL count as known when they registered an installation, are a direct Group member, hold an administrator role, own a Group, are on the deny list or are a bootstrap administrator. An identity-provider role SHALL count as known when it was seen in a user's token or appears in a role mapping.

#### Scenario: Role seen in a token
- **WHEN** carol's token carried the identity-provider role wb-sales and a platform-admin requests the directory
- **THEN** the identity-provider roles include wb-sales
