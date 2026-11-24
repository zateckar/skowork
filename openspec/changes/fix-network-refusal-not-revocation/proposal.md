# Proposal

## Why

Users see "Access to Skowork was revoked for this installation" although no administrator revoked anything. On 2026-11-24 a client logged `heartbeat failed: HTTP 403` followed by `installation locked: access revoked`, and after a restart `registration failed (http_403): HTTP 403`. At the same time an unauthenticated `GET /healthz` on the control-plane address answered `403 Forbidden` with an empty body. The refusal came from the network in front of the control plane (an IP allowlist or WAF that refuses the laptop's current egress address, for example at home rather than in the office), not from the control plane.

Every 403 the control plane sends has a JSON error body with a code (`DENIED`, `FORBIDDEN`, `INSTALLATION_MISMATCH`, ...). The client nevertheless locks the installation on a bare `http_403` from registration, heartbeat or the push handshake. It also locks when a re-registration after a push 403 fails for any reason. A lock lasts until the process restarts, so the client never recovers on its own when the laptop returns to an allowed network.

## What Changes

- Only the control plane's own revocation (registration answering `revoked: true`, or an error with code `DENIED`/`revoked`) locks the installation.
- A 403 without a control-plane error code is a connection failure. The client stays disconnected, retries with the existing backoff, and logs that the network refused the connection to the control plane.
- A push-handshake 403 re-registers to learn the cause and locks only when that registration reports revocation.

Out of scope:
- Control-plane changes, including the stale `revoked_at` after a user deny entry is removed.
- Detecting which network device sent the refusal.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- client/control-channel: Remote Session Revocation.
