# client/local-storage Specification

## Purpose
The user-private encrypted local data store and its lifecycle.

## Requirements

### Requirement: Schema Migration
The client SHALL version its storage schema and migrate existing data forward automatically on upgrade. It MUST take a backup copy before a migration and roll back to that copy if the migration fails.

#### Scenario: Failed migration
- **WHEN** a schema migration fails partway
- **THEN** the client restores the pre-migration copy, starts in a degraded read-only mode, and reports the failure in diagnostics and telemetry

### Requirement: Forget Data
The client SHALL let the user permanently delete selected data from the client: a Thread, a Source, an individual indexed item, or everything. Deletion MUST cascade to derived data such as indexes, embeddings, knowledge-graph facts and caches.

#### Scenario: Delete everything
- **WHEN** the user chooses "Delete all my Skowork data" and confirms
- **THEN** the client removes every file under its data root except the minimal configuration needed to start
- **AND** it restarts in a first-run state
