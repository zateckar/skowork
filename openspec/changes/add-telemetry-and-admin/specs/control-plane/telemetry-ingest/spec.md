# Spec Delta

## Purpose

Defines how the control plane receives client telemetry. It covers authentication, pseudonymization, retention, and forwarding into the organization observability and SIEM stack.

## ADDED Requirements

### Requirement: Authenticated OTLP Ingest
The telemetry ingest SHALL accept OpenTelemetry log records over HTTPS, in OTLP with protobuf encoding. Client telemetry is a stream of event records (client/telemetry), so traces and metrics are not accepted. Each request MUST carry a valid Workbench access token. The ingest MUST reject records whose stated installation identifier is not registered to the token's user.

#### Scenario: Valid batch
- **WHEN** a client posts a telemetry batch with a valid access token and its own installation identifier
- **THEN** the ingest accepts the batch and acknowledges it

#### Scenario: Spoofed installation
- **WHEN** a batch names an installation identifier that belongs to a different user than the token
- **THEN** the ingest rejects the batch and records a security audit event

### Requirement: Level Enforcement At Ingest
The telemetry ingest MUST drop content fields from any record whose sender's effective policy.telemetry.level is not content. Each such drop is counted as a policy violation in the operational metrics.

#### Scenario: Content from a metadata-level client
- **WHEN** a client whose effective policy is metadata sends a record that contains a prompt text field
- **THEN** the ingest removes that field before storage or forwarding and increments the violation counter

### Requirement: Pseudonymization
When pseudonymization is enabled by control-plane configuration, the telemetry ingest SHALL replace user principals and machine names with keyed pseudonyms before forwarding. Only the security-auditor role, through control-plane/admin-console, can re-identify a pseudonym, and each re-identification is audited.

#### Scenario: Forwarded record is pseudonymized
- **WHEN** pseudonymization is enabled and a record is forwarded to the observability stack
- **THEN** the forwarded record contains a stable pseudonym instead of the user principal

### Requirement: Forwarding To Organization Observability
The telemetry ingest SHALL forward accepted telemetry to the configured organization observability and SIEM targets using OTLP. If a target is unavailable, it SHALL buffer records for at least 24 hours.

#### Scenario: Target outage
- **WHEN** the observability target is unavailable for two hours
- **THEN** the ingest keeps accepting client batches and forwards the buffered records once the target recovers

### Requirement: Telemetry Retention
The telemetry ingest SHALL delete telemetry it stores itself after a configurable retention period (default 30 days). Crash reports are kept for at most 90 days.

#### Scenario: Retention expiry
- **WHEN** a stored telemetry record is older than the retention period
- **THEN** it is deleted

### Requirement: Ingest Rate Limiting
The telemetry ingest SHALL rate-limit each installation and reject excess batches with a retry-after indication, so that a misbehaving client cannot degrade ingest for others.

#### Scenario: Flooding client
- **WHEN** an installation exceeds its batch rate limit
- **THEN** the ingest responds with a retry-after indication and the client delays its next batch accordingly
