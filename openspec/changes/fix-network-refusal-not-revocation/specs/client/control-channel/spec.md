# Spec Delta

## MODIFIED Requirements

### Requirement: Remote Session Revocation
The client SHALL stop all Runs, discard its access tokens, and refresh when it receives a revocation message for its user or installation. If the control plane keeps refusing it, the client SHALL remain in a locked state. Only the control plane's own refusal locks the client: a registration that answers revoked, or an error response whose error code is `DENIED` or `revoked`. A 403 response without a control-plane error code (from a proxy, firewall or gateway in front of the control plane) SHALL be treated as a connection failure: the client stays disconnected, keeps retrying with backoff, and does not lock.

#### Scenario: Installation revoked by admin
- **WHEN** an administrator revokes the installation
- **THEN** the client stops active Runs within 10 seconds and shows that access was revoked

#### Scenario: Network refuses the control plane
- **WHEN** the heartbeat, the registration or the push handshake is answered with 403 and an empty or non-Skowork body
- **THEN** the client does not lock, stays disconnected, logs that the network refused the connection to the control plane, and connects again without a restart once the control plane answers
