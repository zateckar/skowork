# Spec Delta

## ADDED Requirements

### Requirement: Session Kept While Offline
A token refresh that fails because the identity provider or the network cannot be reached SHALL NOT end the session. The client keeps the stored refresh token, stays signed in for local use, and retries the refresh when the control channel reconnects. Only a refresh rejected by the identity provider ends the session (Refresh Token Protection And Rotation).

#### Scenario: Identity provider unreachable for a day
- **WHEN** every refresh fails with a network error for 24 hours
- **THEN** the client is still signed in for local use, and the first refresh after the network returns renews the session or, if the identity provider rejects it, enters the sign-in required state
