# client/control-channel Specification

## Purpose
Defines the managed connection between each client and the control plane: registration, liveness, real-time push of commands and changes, version gating, and behaviour while offline.

## Requirements

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
While disconnected, the client SHALL keep every feature that works on local data available for at least 24 hours, with no time limit of its own: Threads, the Knowledge view and knowledge.search over local Sources, notes, settings, Local apps and reading Chats (client/user-messaging Offline Sending owns sending). It MUST NOT start Runs while disconnected. The status bar SHALL say since when the client is offline; features that need the control plane say they are unavailable.

#### Scenario: Off VPN
- **WHEN** the client is in the disconnected state
- **THEN** no Run starts, and local search and Thread reading still work

#### Scenario: A day offline
- **WHEN** the client has been disconnected for 24 hours with a valid cached Policy bundle
- **THEN** Threads, the Knowledge view, notes, Chats and Local apps still open, and the status bar shows when the client went offline

### Requirement: Requests Queued While Offline
A request the user sends while disconnected (Offline Operation), in an existing Thread or as a new Thread, SHALL wait in that Thread as a queued message labelled as waiting for the connection, which the user can edit or remove. Queued messages start their Runs when the client is connected again, in the order they were written.

#### Scenario: Request written offline
- **WHEN** the user sends "Summarise the brake notes" while disconnected, and the client connects again an hour later
- **THEN** the message waits in its Thread labelled as waiting for the connection, and its Run starts after the client connects

### Requirement: Remote Session Revocation
The client SHALL stop all Runs, discard its access tokens, and refresh when it receives a revocation message for its user or installation. If the control plane keeps refusing it, the client SHALL remain in a locked state.

#### Scenario: Installation revoked by admin
- **WHEN** an administrator revokes the installation
- **THEN** the client stops active Runs within 10 seconds and shows that access was revoked

### Requirement: Machine Configuration Sources
The client SHALL use the documented machine registry configuration when its control-plane URL value exists; otherwise it SHALL load the documented JSON configuration beside the running executable, preserving documented defaults and client/identity validation.

#### Scenario: Shortcut starts elsewhere
- **WHEN** the client starts from a different working directory and its installation path contains Unicode characters
- **THEN** it reads the configuration beside its executable and ignores configuration in the working directory

#### Scenario: Registry configuration takes precedence
- **WHEN** the machine registry has a control-plane URL value and an adjacent configuration file also exists
- **THEN** the client validates and uses only the registry configuration, reporting an error if it is invalid

### Requirement: Machine Configuration Names
The machine configuration names are the Skowork registry key `HKLM\SOFTWARE\Policies\Skowork` and the file `skowork.machine.json` beside the executable. The client SHALL read only these names. It MUST NOT read any registry key or configuration file under any other name (rename-to-skowork D2).

#### Scenario: Only another name exists
- **WHEN** a registry key or configuration file under any other name exists, and no Skowork registry key or file exists
- **THEN** the client does not read it and stays unconfigured, reporting the missing Skowork source

### Requirement: Machine Configuration Encoding
The client SHALL accept its machine configuration file in UTF-8 with or without a byte-order mark and UTF-16 in either byte order with a byte-order mark.

#### Scenario: Configuration saved by a Windows editor
- **WHEN** the machine registry has no control-plane URL value and the adjacent configuration is valid JSON saved in any supported encoding
- **THEN** the client loads its settings and uses the documented defaults for omitted optional settings

#### Scenario: Malformed encoding
- **WHEN** the selected configuration contains truncated UTF-16 or invalid Unicode
- **THEN** the client rejects it as invalid configuration

### Requirement: Machine Configuration Diagnostics
When machine configuration cannot be loaded, the client SHALL identify the selected source and distinguish a missing file, an unreadable file, invalid JSON or encoding, and missing required settings. File errors SHALL include the full attempted path without echoing file contents. When the intended file is absent but a file with an extra text-file extension exists beside it, the error SHALL explain the required filename correction without treating that file as configuration.

#### Scenario: Notepad appends a text-file extension
- **WHEN** the documented file is missing but the adjacent file has an extra text-file extension
- **THEN** the error identifies both filenames and tells the administrator to remove the extra extension

#### Scenario: Invalid configuration
- **WHEN** the selected file is malformed, unreadable, or missing required settings
- **THEN** the client stays unconfigured and reports the precise reason and file path without including configuration values
