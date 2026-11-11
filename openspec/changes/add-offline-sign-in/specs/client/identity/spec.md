# Spec Delta

## MODIFIED Requirements

### Requirement: Identity Provider Configuration
The client SHALL read the issuer URL, OAuth client identifier, loopback redirect URI and base scopes from machine configuration, delivered with the control-plane base URL (client/control-channel). It SHALL discover the endpoints and signing keys from the issuer's OpenID Connect discovery document. Only Offline Sign-In Request may add a scope. The user and Policy bundles MUST NOT change the issuer, client identifier or redirect URI.

#### Scenario: Endpoints discovered
- **WHEN** the client starts with an issuer URL in machine configuration
- **THEN** it reads the authorization, token, revocation, end-session and key endpoints from the issuer's discovery document

#### Scenario: Policy changes the issuer
- **WHEN** a Policy bundle contains an issuer URL that differs from machine configuration
- **THEN** the client ignores it and keeps the configured issuer

### Requirement: Refresh Token Protection And Rotation
The client SHALL store the refresh token encrypted at rest under client/local-storage and nowhere else. The stored token MAY be an offline token from Offline Sign-In Request, and it is handled the same way. The client SHALL replace the stored token with the new one returned by every refresh, and SHALL treat a refresh rejected as invalid_grant as the end of the session.

#### Scenario: Rotation
- **WHEN** a refresh returns a new refresh token
- **THEN** the client stores the new token before it uses the new access token, and never presents the old one again

#### Scenario: Client restart
- **WHEN** the client restarts with a stored refresh token
- **THEN** it obtains a new access token without opening the browser

#### Scenario: Offline token rotated
- **WHEN** a refresh with a stored offline token returns a new refresh token
- **THEN** the client stores the new token in place of the old one, as for any refresh token

## ADDED Requirements

### Requirement: Offline Sign-In Request
When policy.identity.offline_sign_in is on, the client SHALL add the offline_access scope to every sign-in request, so the identity provider can issue an offline refresh token. The client SHALL check the type of the refresh token it receives. If the identity provider issued no offline token, the client MUST keep the refresh token it received, MUST NOT fail the sign-in, and SHALL show "Offline sign-in not available" in the sign-in status.

#### Scenario: Offline token issued
- **WHEN** a sign-in completes with the policy on and the identity provider issues an offline token
- **THEN** the stored refresh token is an offline token and the sign-in status shows offline sign-in as active

#### Scenario: Realm does not issue an offline token
- **WHEN** a sign-in completes with the policy on and the identity provider returns a normal refresh token
- **THEN** the user is signed in, the client keeps the normal refresh token, and the sign-in status shows "Offline sign-in not available"

### Requirement: Offline Session Lifetime
The client SHALL NOT impose its own lifetime on an offline session. The session lasts while the identity provider accepts refreshes, under the realm's offline session limits. The session ends on Sign out, when a refresh is rejected as invalid_grant, or when Offline Sign-In Policy turns the feature off. Each end enters the sign-in required state.

#### Scenario: Start after a long absence
- **WHEN** the client starts 20 days after its last use and the identity provider accepts the stored offline token
- **THEN** it obtains an access token without opening the browser

#### Scenario: Session ended at the identity provider
- **WHEN** the client starts and the identity provider rejects the stored offline token as invalid_grant
- **THEN** the client deletes the token, enters the sign-in required state and does not open the browser on its own

### Requirement: Offline Sign-In Policy
The client SHALL apply policy.identity.offline_sign_in, with values on or off and default on. While it is on, Offline Sign-In Request applies at each sign-in. When the effective policy sets it to off, the client SHALL at the next policy update revoke the stored offline token at the identity provider, delete it and enter the sign-in required state without opening the browser.

#### Scenario: Policy turned off
- **WHEN** an administrator sets policy.identity.offline_sign_in to off and the client receives the policy
- **THEN** the stored offline token is revoked and deleted, and the client shows the sign-in required state

#### Scenario: Policy off at sign-in
- **WHEN** the user signs in while the effective policy sets policy.identity.offline_sign_in to off
- **THEN** the client does not add offline_access to the sign-in request

#### Scenario: Policy off at start
- **WHEN** the client starts with a stored offline token and the cached policy sets policy.identity.offline_sign_in to off
- **THEN** the client revokes and deletes the token and enters the sign-in required state before it sends any refresh request

#### Scenario: Revocation fails
- **WHEN** the revocation request for the stored offline token fails because the identity provider cannot be reached
- **THEN** the client still deletes the token and enters the sign-in required state, and it records a security event "offline_revoke_failed"

#### Scenario: Normal token kept when policy is off
- **WHEN** the effective policy sets policy.identity.offline_sign_in to off and the stored refresh token is a normal refresh token
- **THEN** the client keeps that token unchanged, because only an offline token is revoked

### Requirement: Offline Token Type
The client SHALL determine the type of a stored refresh token from its `typ` claim: `Offline` for an offline token, and any other value for a normal refresh token. The sign-in status SHALL be derived from the stored token and the policy, so it is the same after a restart. The identity status card SHALL show the status.

#### Scenario: Status after restart
- **WHEN** the client restarts with a stored offline token and the policy on
- **THEN** the identity status card shows offline sign-in as active

#### Scenario: Status values
- **WHEN** the policy is on and the stored token is a normal refresh token
- **THEN** the status is "Offline sign-in not available"
- **WHEN** the policy is off, or no session is stored
- **THEN** the status is off and nothing is shown
