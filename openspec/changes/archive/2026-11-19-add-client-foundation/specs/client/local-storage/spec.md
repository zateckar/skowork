# Spec Delta

## Purpose

Defines where and how the client stores user data so that it stays in the user's profile, is readable only by that user, survives upgrades, and can be deleted on request.

## ADDED Requirements

### Requirement: Data Location In User Profile
The client SHALL store all persistent user data under a single data root inside the user's profile. The default is the local application data folder, and policy.storage.root can override it. Data MUST NOT be written to machine-wide locations, other than optional read-only shared ML models.

#### Scenario: Default data root
- **WHEN** no storage policy is set
- **THEN** all databases, logs, skills, browser profile and caches are created under the user's local application data folder in a Skowork directory

#### Scenario: Policy override
- **WHEN** policy.storage.root points to another folder inside the user's profile
- **THEN** the client uses that folder as the data root on next start

### Requirement: Owner-Only Access
The client SHALL restrict file-system access to its data root to the owning user and the local SYSTEM account. SYSTEM is the one accepted exception, because profile backup needs it. The client MUST remove inherited permissions that would grant access to any other account. The default for policy.storage.root is the Skowork folder in the user's local application data.

#### Scenario: Another user reads the data root
- **WHEN** a different non-administrator user tries to open a file in the data root
- **THEN** access is denied

### Requirement: Encryption At Rest
The client SHALL encrypt its databases and any stored secrets. The keys are protected by the operating system's per-user data protection, so the data can be decrypted only in that user's logon context.

#### Scenario: Copied database is unreadable
- **WHEN** a database file from the data root is copied to another machine or opened by another account
- **THEN** its contents cannot be read without the owning user's logon context

#### Scenario: Profile migration
- **WHEN** the user's profile is migrated to a new machine by the organization migration process for the same domain account
- **THEN** the client opens its existing databases on the new machine without data loss

### Requirement: Schema Migration
The client SHALL version its storage schema and migrate existing data forward automatically on upgrade. It MUST take a backup copy before a migration and roll back to that copy if the migration fails.

#### Scenario: Failed migration
- **WHEN** a schema migration fails partway
- **THEN** the client restores the pre-migration copy, starts in a degraded read-only mode, and reports the failure in diagnostics and telemetry

### Requirement: Crash-Safe Writes
The client SHALL write user data transactionally, so that a crash or power loss never leaves a partially written record visible after restart.

#### Scenario: Power loss during write
- **WHEN** the machine loses power while a Thread message is being saved
- **THEN** after restart the Thread contains either the complete message or no trace of it

### Requirement: Forget Data
The client SHALL let the user permanently delete selected data from the client: a Thread, a Source, an individual indexed item, or everything. Deletion MUST cascade to derived data such as indexes, embeddings, knowledge-graph facts and caches.

#### Scenario: Delete everything
- **WHEN** the user chooses "Delete all my Skowork data" and confirms
- **THEN** the client removes every file under its data root except the minimal configuration needed to start
- **AND** it restarts in a first-run state

### Requirement: Storage Quota
The client SHALL enforce a configurable total size limit for its data root (policy.storage.max_size_gb, default 10 GB). When the limit is approached, it SHALL warn the user and stop non-essential growth (caches first, then indexing).

#### Scenario: Quota approached
- **WHEN** the data root reaches 90% of its limit
- **THEN** the client clears expired caches, pauses new indexing, and shows a warning with a breakdown of usage by category
