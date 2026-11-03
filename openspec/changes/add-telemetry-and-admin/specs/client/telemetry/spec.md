# Spec Delta

## Purpose

Defines what operational telemetry the client collects and sends at each telemetry level. It also defines how telemetry is buffered and how users can see exactly what was sent about them.

## ADDED Requirements

### Requirement: Telemetry Levels
The client SHALL collect telemetry at the level set by policy.telemetry.level, which is one of off, metadata or content. When policy does not set a level, the client SHALL use metadata. At the off level, the client MUST NOT send any telemetry except a heartbeat through client/control-channel.

#### Scenario: Default level
- **WHEN** the user's policy bundle does not set policy.telemetry.level
- **THEN** the client collects and sends metadata-level telemetry only

#### Scenario: Telemetry off
- **WHEN** policy.telemetry.level is off
- **THEN** the client sends no traces, metrics, logs or crash reports to the control plane

### Requirement: Metadata Level Contents
At the metadata level, the client SHALL send only operational data:
- Run counts and durations
- tool ids, risk classes and outcomes
- approval decision counts
- token usage per model role
- error codes
- client and OS versions
- footprint and performance metrics

It MUST NOT send message text, file content, file names, URLs beyond the domain, email or calendar data, transcripts or screenshots.

#### Scenario: Tool call recorded without content
- **WHEN** a Run calls files.read on a document
- **THEN** the telemetry records the tool id, risk class, outcome and duration
- **AND** it contains neither the file path nor any file content

#### Scenario: Web fetch domain only
- **WHEN** a Run calls web.fetch for a URL with a path and query string
- **THEN** the telemetry records only the domain of that URL

### Requirement: Enforcement And Decision Metadata
At the metadata level, the client SHALL also send two things. The first is the network enforcement level reported by client/tool-sandbox. The second is a policy decision record from client/policy-enforcement for each recorded decision: tool id, Risk class, matched rule id, policy version and outcome. These records MUST NOT include paths, URLs beyond the domain, or tool arguments.

#### Scenario: Policy decision recorded
- **WHEN** a tool call is denied by a filesystem rule
- **THEN** the telemetry records the tool id, Risk class, matched rule id, policy version and the outcome deny, without the path

#### Scenario: Enforcement level reported
- **WHEN** the client starts a sandboxed process and reports the network enforcement level as proxy-only
- **THEN** the next telemetry batch contains that enforcement level

### Requirement: Content Level
The client SHALL include content (prompts, model responses and tool arguments) in telemetry only when policy.telemetry.level is content. At content level, the UI MUST show a persistent indicator that content is being recorded for the organization.

#### Scenario: Content level enabled
- **WHEN** the user's policy sets policy.telemetry.level to content
- **THEN** the main window and tray tooltip show a "content telemetry active" indicator for as long as the level is in force

#### Scenario: Metadata level excludes content
- **WHEN** policy.telemetry.level is metadata
- **THEN** no prompt, model response or tool argument text appears in any telemetry record

### Requirement: Secret Redaction
The client MUST remove access and refresh tokens, authorization codes, passwords and values matching policy.telemetry.redaction_patterns from all telemetry at every level before it is buffered or sent. That key is a list of regular expressions, and it adds to the built-in token and password patterns, which are always applied. The default is an empty list.

#### Scenario: Token in an error message
- **WHEN** an error message contains a bearer token
- **THEN** the telemetry record contains a redaction marker in place of the token

#### Scenario: Organisation-specific pattern
- **WHEN** policy.telemetry.redaction_patterns contains a pattern for internal personnel numbers and an error message contains one
- **THEN** the telemetry record contains a redaction marker in place of that number

### Requirement: Redaction Pattern Limits
The client SHALL match policy.telemetry.redaction_patterns in time linear in the text, using ECMAScript syntax without back-references or lookaround. It MUST skip a pattern outside that syntax, or one too large to match, and report it in its diagnostic log. If matching one text exceeds a fixed work budget, the client MUST replace the whole text with the redaction marker.

#### Scenario: Pattern with nested quantifiers
- **WHEN** policy.telemetry.redaction_patterns contains a pattern such as `(a|b|ab)*(a|b)*c` and a long error message is recorded
- **THEN** the record is built within the work budget and the client stays responsive

#### Scenario: Unsupported pattern
- **WHEN** a pattern uses a back-reference
- **THEN** the client skips that pattern, applies the others, and logs that one pattern was ignored

### Requirement: Offline Telemetry Buffer
The client SHALL buffer telemetry in encrypted local storage while the control plane is unreachable and send it when connectivity returns. The buffer is capped by policy.telemetry.buffer_max_mb (default 20 MB). When the cap is reached, the oldest records are discarded first.

#### Scenario: Reconnect after offline period
- **WHEN** the client reconnects after being offline
- **THEN** it sends the buffered telemetry in order and removes each record from the buffer once the ingest acknowledges it

#### Scenario: Buffer full
- **WHEN** the buffer reaches its cap while offline
- **THEN** the client discards the oldest records and counts the number discarded in the next telemetry batch

### Requirement: Crash Reports
When the core or a worker process crashes, the client SHALL capture a crash report if policy.telemetry.crash_reports is true (the default). At telemetry level off, crash reports are never captured. Before sending, the client strips user content where the dump format allows. Each report is listed in the Telemetry viewer for one hour before it is sent, so the user can view or delete it first. After that hour it is sent at the next connection.

#### Scenario: Worker crash
- **WHEN** a worker process crashes while crash reports are enabled
- **THEN** a crash report is stored, listed in the Telemetry viewer, and sent when the control plane is reachable

#### Scenario: Crash reports disabled
- **WHEN** policy.telemetry.crash_reports is false
- **THEN** no crash dump is written or sent

### Requirement: Telemetry Viewer
The client SHALL provide a Telemetry viewer in its settings. The viewer shows the current telemetry level and the records sent or waiting to be sent in the last 30 days, in human-readable form.

#### Scenario: User inspects telemetry
- **WHEN** the user opens the Telemetry viewer
- **THEN** they see the active level, the policy that set it, and a list of sent and buffered records with their fields

### Requirement: Telemetry Identity
The client SHALL attach the installation identifier and the user principal to every telemetry batch, using the authenticated connection to the ingest. Records MUST NOT carry any other personal identifier.

#### Scenario: Batch attribution
- **WHEN** the client sends a telemetry batch
- **THEN** the batch is authenticated with the user's access token and carries the installation identifier
