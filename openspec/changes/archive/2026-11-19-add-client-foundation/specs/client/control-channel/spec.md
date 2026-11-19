# Spec Delta

## Purpose

Defines the managed connection between each client and the control plane: registration, liveness, real-time push of commands and changes, version gating, and behaviour while offline.

## ADDED Requirements

### Requirement: Control-Plane Discovery
The client SHALL read the control-plane base URL from machine configuration provided by the organization software distribution system. The URLs of all other control-plane services, including the LLM gateway, SHALL be taken from the registration response. The client MUST NOT let the user or a Policy bundle change the control-plane base URL.

#### Scenario: Gateway URL from registration
- **WHEN** registration succeeds
- **THEN** the client uses the gateway, policy, registry and telemetry URLs from the registration response for all later requests

### Requirement: Device Registration
After authenticating, the client SHALL register with the control plane. Registration reports a stable installation identifier, the machine name, the user principal, the client version and channel, the operating-system build and the hardware class.

#### Scenario: First start
- **WHEN** the client authenticates for the first time on a machine
- **THEN** it creates a random installation identifier, stores it in the data root, and registers it with the control plane

### Requirement: Heartbeat
While connected, the client SHALL send a heartbeat to the control plane at the interval the control plane specifies (default 5 minutes). The heartbeat includes the current policy version and coarse status (idle, running, paused).

#### Scenario: Heartbeat carries policy version
- **WHEN** the client sends a heartbeat
- **THEN** the heartbeat includes the version identifier of the policy bundle currently in force

### Requirement: Push Connection
The client SHALL maintain a persistent, authenticated, server-to-client message connection to the control plane. It reconnects automatically with jittered exponential backoff capped at 5 minutes.

#### Scenario: Server push received
- **WHEN** the control plane pushes a message (for example policy-changed or remote-task)
- **THEN** the client receives it within 5 seconds while connected and acknowledges it

#### Scenario: Connection lost
- **WHEN** the push connection drops
- **THEN** the client reconnects using jittered exponential backoff and, after reconnecting, requests any messages it missed

### Requirement: Push Connection Token Renewal
The push connection SHALL authenticate with an access token at the handshake. Because access tokens live only 5 minutes, the client SHALL send each refreshed access token over the open connection in an auth-renew message before the previous one expires. The control plane closes a connection whose last presented token expired more than 30 seconds ago.

#### Scenario: Long-lived connection
- **WHEN** the push connection stays open for an hour
- **THEN** the client has sent at least 11 auth-renew messages and the connection has not been closed for an expired token

### Requirement: Client Messages Over The Channel
The client SHALL send its messages to the control plane over the same authenticated connection. They are an ack for each push message, auth-renew (Push Connection Token Renewal), plus remote-task-claim, remote-task-accept, remote-task-decline and remote-task-status as defined by control-plane/task-dispatch. Each message carries the id of the push message it answers. Messages that cannot be sent while disconnected are queued in local storage and sent after reconnecting.

#### Scenario: Remote task accepted while offline
- **WHEN** the user accepts a Remote task and the connection drops before remote-task-accept is sent
- **THEN** remote-task-accept is stored locally and sent after reconnecting, with the original push message id

### Requirement: Push Message Integrity
The client SHALL act only on push messages that arrive over the authenticated connection and are addressed to its user or installation. It MUST ignore and log any message that fails validation.

#### Scenario: Message for another installation
- **WHEN** a push message names a different installation identifier
- **THEN** the client ignores it and records a diagnostic event

### Requirement: Minimum Version Gate
The client SHALL compare its version with the minimum supported version that the control plane returns at registration or pushes in a minimum-version message. Below that version, it MUST disable all LLM-backed features and show the user an instruction to update through the organization software portal. A pushed minimum version applies immediately.

#### Scenario: Outdated client
- **WHEN** the control plane reports a minimum version higher than the client's version
- **THEN** the client blocks new Runs, keeps existing Threads readable, and shows an update notice

#### Scenario: Minimum version raised while running
- **WHEN** a connected client receives a minimum-version push above its own version
- **THEN** it applies the gate at once: no new Run starts, active Runs may finish, and the update notice is shown

### Requirement: Connection State
The client SHALL be in the disconnected state whenever no valid access token can be obtained (client/identity) or the push connection has been down for more than 60 seconds. It is connected otherwise. All other capabilities use this one state, and none defines its own.

#### Scenario: Sign-in required
- **WHEN** client/identity enters the sign-in required state
- **THEN** the client enters the disconnected state immediately

#### Scenario: Short network blip
- **WHEN** the push connection drops and is re-established within 30 seconds while the token stays valid
- **THEN** the client never enters the disconnected state

### Requirement: Offline Operation
While disconnected, the client SHALL keep local features available: reading Threads, searching local data, and editing local drafts. It MUST disable features that need the control plane, including starting new Runs.

#### Scenario: Off VPN
- **WHEN** the client is in the disconnected state
- **THEN** new Runs are disabled, and local search and Thread reading still work

### Requirement: Remote Session Revocation
The client SHALL stop all Runs, discard its access tokens, and refresh when it receives a revocation message for its user or installation. If the control plane keeps refusing it, the client SHALL remain in a locked state.

#### Scenario: Installation revoked by admin
- **WHEN** an administrator revokes the installation
- **THEN** the client stops active Runs within 10 seconds and shows that access was revoked
