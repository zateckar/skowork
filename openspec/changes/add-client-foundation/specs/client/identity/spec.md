# Spec Delta

## Purpose

Defines how the client signs the user in at the organization identity provider (Keycloak) with OAuth 2.0 Authorization Code and PKCE, and how it keeps the resulting short-lived access tokens fresh for every call to the control plane.

## ADDED Requirements

### Requirement: Identity Provider Configuration
The client SHALL read the identity provider's issuer URL, its OAuth client identifier, the loopback redirect URI and the requested scopes from machine configuration, delivered with the control-plane base URL (client/control-channel). It SHALL discover the provider's endpoints and signing keys from the issuer's OpenID Connect discovery document. The user and Policy bundles MUST NOT change them.

#### Scenario: Endpoints discovered
- **WHEN** the client starts with an issuer URL in machine configuration
- **THEN** it reads the authorization, token, revocation, end-session and key endpoints from the issuer's discovery document

### Requirement: Authorization Code Sign-In With PKCE
The client SHALL sign the user in with the OAuth 2.0 Authorization Code flow as a public client, using PKCE with the S256 method, a fresh state value and an OpenID Connect nonce for every attempt. The sign-in page opens in the user's default browser. The client MUST NOT see, prompt for or store the user's password, and MUST NOT host the identity provider's sign-in page itself.

#### Scenario: First sign-in
- **WHEN** the client starts and holds no refresh token
- **THEN** it opens the identity provider's sign-in page in the default browser and shows the signing-in state until the redirect arrives

#### Scenario: State mismatch
- **WHEN** a redirect arrives whose state does not match a pending sign-in
- **THEN** the client discards it, makes no token request and records a security event

### Requirement: Loopback Redirect During Sign-In Only
The client SHALL receive the authorization response on the loopback redirect URI from machine configuration (for example http://localhost:4200/auth/callback). It SHALL listen on the loopback interface only while a sign-in is pending, and stop when the redirect arrives, the sign-in is cancelled or 5 minutes pass. The browser gets a short page saying the tab can be closed; other paths are refused.

#### Scenario: Redirect received
- **WHEN** the browser completes sign-in and navigates to the redirect URI
- **THEN** the client receives the authorization code within 2 seconds and the loopback listener is closed

#### Scenario: No listener outside sign-in
- **WHEN** the client is signed in and no sign-in is pending
- **THEN** the client has no listening socket

#### Scenario: Port in use
- **WHEN** the redirect port is already taken by another process at sign-in
- **THEN** the client does not open the browser, stays in the sign-in required state and tells the user the port is in use

#### Scenario: Foreign response while a sign-in is pending
- **WHEN** a request arrives on the redirect path whose state matches no pending sign-in
- **THEN** the client refuses it as in State mismatch and keeps listening for the real redirect

### Requirement: Sign-In Required State
When no valid refresh token is available and silent renewal is impossible, the client SHALL enter the sign-in required state. It SHALL show it in the tray and as a Question in client/work-inbox with a Sign in choice, and SHALL open the browser only at first start or when the user chooses Sign in. client/control-channel treats this state as disconnected.

#### Scenario: Refresh token expired over a holiday
- **WHEN** the client starts and its refresh token is rejected as invalid or expired
- **THEN** it deletes the stored refresh token, enters the sign-in required state and does not open the browser on its own

### Requirement: Access Token Freshness Before Every Control-Plane Call
The client SHALL keep access tokens in memory only. Before every request and every push-connection handshake to the control plane, it SHALL ensure the access token remains valid for at least 60 seconds, and refresh it with the refresh token otherwise. Concurrent callers SHALL share a single refresh. When the control plane answers 401, the client SHALL refresh once and retry the request once.

#### Scenario: Token about to expire
- **WHEN** a component calls the control plane and the access token expires in less than 60 seconds
- **THEN** the client refreshes the token first and sends the request with the new token

#### Scenario: Parallel calls during refresh
- **WHEN** three components call the control plane while a refresh is in progress
- **THEN** exactly one token request is sent and all three calls use its result

#### Scenario: Token rejected by the control plane
- **WHEN** the control plane answers 401 to a request carrying an unexpired token
- **THEN** the client refreshes once and retries once, and on a second 401 reports the call as failed and enters the sign-in required state

### Requirement: Refresh Token Protection And Rotation
The client SHALL store the refresh token encrypted at rest under client/local-storage and nowhere else. It SHALL replace the stored refresh token with the new one returned by every refresh, and SHALL treat a refresh rejected as invalid_grant as the end of the session.

#### Scenario: Rotation
- **WHEN** a refresh returns a new refresh token
- **THEN** the client stores the new token before it uses the new access token, and never presents the old one again

#### Scenario: Client restart
- **WHEN** the client restarts with a stored refresh token
- **THEN** it obtains a new access token without opening the browser

### Requirement: Server Identity Verification
The client SHALL connect to the identity provider and the control plane only over TLS, validating their certificates against the Windows certificate store.

#### Scenario: Untrusted certificate
- **WHEN** the identity provider's certificate does not chain to a trusted root
- **THEN** the client refuses the connection and sends no authorization code or refresh token

### Requirement: ID Token Verification And User Binding
The client SHALL verify the ID token returned at sign-in: signature against the provider's published keys, issuer, an audience that includes its client identifier, an authorized party equal to its client identifier, nonce and validity period. It SHALL bind the data root to the ID token's subject at first sign-in and MUST refuse tokens for a different subject afterwards.

#### Scenario: Different person signs in
- **WHEN** a sign-in completes for a subject other than the one the data root is bound to
- **THEN** the client discards the tokens, stays in the sign-in required state, records a security event and tells the user which account the data belongs to

### Requirement: User Change Detection
The client SHALL refuse to open a data root bound to another Windows user (client/local-storage) and SHALL discard all tokens and cached policy when the bound identity no longer matches.

#### Scenario: Data root bound to another user
- **WHEN** the client starts and finds that its data root is bound to a different Windows user
- **THEN** it refuses to open that data and reports the mismatch

### Requirement: Sign-Out
The client SHALL let the user sign out. Signing out revokes the refresh token at the identity provider, deletes the stored refresh token, discards access tokens, stops active Runs and enters the sign-in required state. Local data stays.

#### Scenario: User signs out
- **WHEN** the user chooses Sign out
- **THEN** the refresh token is revoked and deleted, and the next control-plane call requires a new sign-in
