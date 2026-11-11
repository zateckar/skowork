# Spec Delta

## ADDED Requirements

### Requirement: Offline Session Tokens
The control plane SHALL accept an access token that the identity provider issued from an offline session under the same rules as any other access token. It MUST NOT request, store or use offline or refresh tokens, and MUST NOT use the sid claim to decide trust. Requests from users and installations on the deny list stay refused, whatever the token.

#### Scenario: Token after a long offline session
- **WHEN** a client presents an access token refreshed from an offline session, valid for 5 minutes, for an allowed client
- **THEN** the request is authenticated as the token's subject

#### Scenario: Revoked installation with a valid token
- **WHEN** an installation on the deny list presents a fresh access token from its offline session
- **THEN** the request is refused with 403
