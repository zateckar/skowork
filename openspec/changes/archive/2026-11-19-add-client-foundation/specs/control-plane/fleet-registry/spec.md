# Spec Delta

## Purpose

Defines how the control plane tracks every client installation, delivers push messages to connected clients, and enforces the minimum supported client version.

## ADDED Requirements

### Requirement: Installation Registry
The fleet registry SHALL record every registered installation: installation identifier, user principal (taken only from the access token's preferred_username; a user principal in the request is ignored), the user's Groups as determined by control-plane/identity at the most recent registration or heartbeat, machine name, client version and channel, operating-system build, hardware class, first-seen and last-seen times, and current policy version.

#### Scenario: Registration recorded
- **WHEN** a client registers
- **THEN** the registry creates or updates the installation record, sets last-seen to the current time, and stores the user's current Groups

#### Scenario: User identity from the token only
- **WHEN** a client registers with a request body that names a user principal different from the access token's preferred_username
- **THEN** the body value is ignored, and the record holds the token's preferred_username

#### Scenario: Group membership changes
- **WHEN** an administrator adds a user to the Group "developers"
- **THEN** the installation record lists "developers" after the next registration or heartbeat

### Requirement: Reported Enforcement And Roles
The fleet registry SHALL also record, per installation, the network enforcement level the client reports with its heartbeat (proxy-only or os-enforced, client/tool-sandbox) and the identity-provider roles of the user's most recent access token. control-plane/policy-service uses both in Policy Simulation.

#### Scenario: Enforcement level reported
- **WHEN** a client's heartbeat reports the network enforcement level proxy-only
- **THEN** the installation record holds proxy-only, and simulating that user's policy counts the installation as proxy-only

### Requirement: Liveness Tracking
The fleet registry SHALL mark an installation as offline when it has missed three consecutive heartbeat intervals. It shows online, offline and last-seen state to administrators.

#### Scenario: Laptop goes offline
- **WHEN** an installation sends no heartbeat for three intervals
- **THEN** its status changes to offline

### Requirement: Push Delivery
The fleet registry SHALL deliver push messages addressed to a user, an installation, a Group or every installation to every connected matching client. It SHALL store undelivered messages for the push retention period, a control-plane server setting (default 7 days), and deliver them on reconnect. A message's own expiry, if earlier, takes precedence.

#### Scenario: Message to offline client
- **WHEN** a push message targets an installation that is offline
- **THEN** the message is stored and delivered when that installation reconnects, unless it has expired

#### Scenario: Group fan-out
- **WHEN** a policy-changed message targets the Group "developers"
- **THEN** every connected client of a member of that Group receives the message

### Requirement: Push Message Types
The fleet registry SHALL push to clients only these registered message types:
- revocation and minimum-version (this capability);
- policy-changed (control-plane/policy-service);
- skills-changed and skill-revoked (control-plane/skill-registry);
- remote-task, remote-task-claimed and remote-task-cancelled (control-plane/task-dispatch).

Client-to-server types are listed in Client Message Routing. New types are added through a change. Unknown types MUST be ignored and logged.

#### Scenario: MCP catalog change
- **WHEN** an MCP catalog entry changes
- **THEN** affected clients receive policy-changed from control-plane/policy-service, because the catalog is delivered inside the Policy bundle

### Requirement: Client Message Routing
The fleet registry SHALL accept only these client-to-server types: ack and auth-renew (client/control-channel), and remote-task-claim, remote-task-accept, remote-task-decline and remote-task-status, which it routes to control-plane/task-dispatch. Each message is accepted only from the installation the referenced push was delivered to (Delivered-Message References).

#### Scenario: Claim routed to dispatch
- **WHEN** an installation sends remote-task-claim for a remote-task message it received
- **THEN** the claim is forwarded to task-dispatch, which answers the other installations with remote-task-claimed

#### Scenario: Unknown message type
- **WHEN** a client receives a push message with an unregistered type
- **THEN** it ignores the message, acknowledges it, and records a diagnostic event

### Requirement: Delivered-Message References
The `ref` of a client-to-server message SHALL be the id of a push message on that installation's own push connection (ids are per installation). The fleet registry MUST reject a message whose `ref` names a push message that was never delivered to that installation, with the error code NOT_DELIVERED, and MUST record a security audit event.

#### Scenario: Forged claim
- **WHEN** an installation sends a claim referencing a push message that was never delivered to it
- **THEN** the message is rejected with NOT_DELIVERED and a security audit event is recorded

#### Scenario: Ack for an undelivered message
- **WHEN** an installation sends an ack referencing a push message id that was never delivered to it
- **THEN** the message is rejected with NOT_DELIVERED and a security audit event is recorded

### Requirement: Minimum Supported Version
The fleet registry SHALL let administrators set a minimum supported client version per channel, and SHALL return it to clients at registration and through push. When no minimum is set for a channel, the value is "0.0.0", which means no minimum.

#### Scenario: No minimum set
- **WHEN** a client on a channel with no minimum version registers
- **THEN** the returned minimum version is "0.0.0" and the version gate does not block the client

#### Scenario: Raising the minimum version
- **WHEN** an administrator raises the minimum version for the stable channel
- **THEN** connected clients below that version receive the new minimum through push and apply the version gate

### Requirement: Installation Revocation
The fleet registry SHALL let administrators revoke an installation or all installations of a user. Revoking an installation adds an installation entry to the control-plane/identity deny list. Revoking a user adds a user entry to it, which also blocks any new registration by that user. A revocation message is pushed, and the control plane refuses further requests from revoked installations and users (control-plane/identity deny list).

#### Scenario: Revoke installation
- **WHEN** an administrator revokes an installation
- **THEN** an installation deny-list entry is added, a revocation message is pushed to it, and its subsequent requests are refused

#### Scenario: Revoke user
- **WHEN** an administrator revokes a user
- **THEN** a user deny-list entry is added, a revocation message is pushed, every push connection of that user is closed with close code 4403, and a later registration by that user is refused

### Requirement: Registry Data Retention
The fleet registry SHALL delete installation records that have not been seen for longer than the registry retention period, a control-plane server setting (default 180 days).

#### Scenario: Stale installation
- **WHEN** an installation has not been seen for longer than the retention period
- **THEN** its record is deleted from the registry
