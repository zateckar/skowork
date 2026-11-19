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

### Requirement: Identity Provider Trust
The control plane SHALL accept only bearer access tokens from the configured identity provider issuer. It SHALL verify the signature against the issuer's published keys, the issuer, an audience in the configured audiences, an authorized party in the configured allowed clients, and the validity period with at most 30 seconds of clock skew. Audiences and allowed clients are server settings that default to the desktop client's identifier.

#### Scenario: Valid token
- **WHEN** a client presents an unexpired access token signed by the configured issuer for an allowed client
- **THEN** the request is authenticated as the token's subject, with the user name taken from preferred_username

#### Scenario: Token from another issuer or client
- **WHEN** a request carries a token from a different issuer, for a different audience or from a client that is not allowed
- **THEN** the request is rejected with 401 and an audit event is recorded

### Requirement: Signing Key Refresh
When a token names an unknown signing key, the control plane SHALL reload the issuer's published keys before rejecting it. Forced reloads are limited to one per 60 seconds.

#### Scenario: Key rotated at the identity provider
- **WHEN** a request carries a token signed with a key the control plane has not seen, and no forced reload happened within the last 60 seconds
- **THEN** the control plane reloads the issuer's keys and authenticates the request if the key is now published

#### Scenario: Unknown key flood
- **WHEN** requests keep arriving with tokens that name unknown keys
- **THEN** the keys are reloaded at most once per 60 seconds, and the other requests are rejected with 401

### Requirement: Short Access Token Lifetime
The control plane SHALL reject access tokens whose lifetime (expiry minus issued-at) exceeds 10 minutes, a control-plane server setting. The identity provider issues access tokens valid for 5 minutes.

#### Scenario: Long-lived token
- **WHEN** a request carries a token valid for 60 minutes
- **THEN** the request is rejected with 401

### Requirement: Groups And Admin Roles
The control plane SHALL determine the caller's Groups as the union of the Groups mapped from the token's roles claim, the Groups the user is assigned to directly, and the Group "everyone". Role mappings and assignments are administered in control-plane/admin-console. The admin roles defined by control-plane/admin-console SHALL be assigned to users in the control plane; users in the bootstrap-admins server setting always hold platform-admin.

#### Scenario: Role mapped to a Group
- **WHEN** a user whose token lists a role mapped to "developers" calls the control plane
- **THEN** the request is evaluated with the Groups "developers" and "everyone"

#### Scenario: Direct assignment
- **WHEN** an administrator assigns a user to "region-eu"
- **THEN** that user's next request is evaluated with "region-eu", whatever the token's roles

#### Scenario: Bootstrap administrator
- **WHEN** a user named in the bootstrap-admins setting calls the admin API on a fresh installation
- **THEN** the request is evaluated with the platform-admin role

### Requirement: Deny List
The control plane MUST refuse every request from users and installations on its deny list with 403, whatever the token. Accounts disabled in the directory are refused by the identity provider, which stops issuing and refreshing their tokens.

#### Scenario: Denied user
- **WHEN** an administrator puts a user on the deny list
- **THEN** the user's next request is refused and the fleet registry sends a revocation to that user's clients

### Requirement: Authentication Audit
The control plane SHALL record an audit event for every refused request and for the first request of every identity-provider session. Each event holds the subject, user name, installation identifier when known, source address, outcome and reason.

#### Scenario: Refusal audited
- **WHEN** a request is refused for an invalid token
- **THEN** an audit event with the refusal reason is stored and is visible in the admin console

### Requirement: Production Issuer Guard
In production the control plane MUST refuse to start when the configured issuer does not use https. An identity provider that is unreachable at startup MUST NOT stop the service: it logs a warning and fetches the keys when the first token needs them.

#### Scenario: Production deployment with an http issuer
- **WHEN** the service starts with the production environment setting and an http issuer
- **THEN** it refuses to start and logs the reason

#### Scenario: Identity provider down at startup
- **WHEN** the service starts and the identity provider cannot be reached
- **THEN** it starts, logs a warning, and authenticates tokens once the keys can be fetched
