# Spec Delta

## MODIFIED Requirements

### Requirement: Retries With Backoff
The client SHALL retry requests that fail with RATE_LIMIT, MODEL_UNAVAILABLE or TIMEOUT up to 3 times, using jittered exponential backoff and honouring any retry delay the gateway sends. MODEL_UNAVAILABLE with reason "role not granted" is not retried. AUTH_EXPIRED triggers one token refresh and then one retry. Other errors are not retried. A caller with its own request-spacing policy MAY explicitly request a single gateway attempt; for that request the client SHALL NOT automatically resend after a transient failure, token refresh, or Tool-call validation. A later caller-initiated request remains subject to current authorization.

#### Scenario: Transient outage
- **WHEN** the first request fails with MODEL_UNAVAILABLE and the second succeeds
- **THEN** the Run continues without showing an error, and the retry is recorded in the Activity log

#### Scenario: Rate-limited background feature
- **WHEN** a caller selects a single gateway attempt and the gateway returns a retryable failure or rejects the token
- **THEN** the client returns the failure without a second gateway request and the caller controls any later attempt
