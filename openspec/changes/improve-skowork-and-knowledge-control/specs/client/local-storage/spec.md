# Spec Delta

## MODIFIED Requirements

### Requirement: Forget Data
The client SHALL let the user permanently delete selected data: a Thread, a Source, an indexed item, or everything. Deletion MUST cascade to derived indexes, embeddings, knowledge facts, summaries and caches. Shared content follows the withdrawal rules of client/knowledge-graph and control-plane/team-knowledge. Pending remote cleanup retains only the minimal encrypted withdrawal records defined by Pending Shared Data Removal, never the forgotten content.

#### Scenario: Delete everything
- **WHEN** the user chooses Delete all my SkoWork data and confirms
- **THEN** the client removes all local content and derived data, retaining only minimal startup configuration and any pending shared-data withdrawal records
- **AND** it restarts in a first-run state that identifies any remote removal still pending

### Requirement: Schema Migration
The client SHALL version its storage schema and migrate existing data forward automatically on upgrade. It MUST take an encrypted, consistent backup before a migration. A failed migration MUST leave the database at its pre-migration state, either by restoring that copy or by rolling back all migrations in one atomic transaction without replacing a database that another writer may be using.

#### Scenario: Failed migration
- **WHEN** a schema migration fails partway
- **THEN** the client retains the pre-migration data, starts in a degraded read-only mode, and reports the failure in diagnostics and telemetry

#### Scenario: An old writer overlaps a knowledge migration
- **WHEN** an older client attempts to write while a knowledge database snapshot and migration are in progress
- **THEN** its write cannot be incorporated between the backup and the migration commit, and a successfully migrated database rejects its subsequent writes

## ADDED Requirements

### Requirement: Pending Shared Data Removal
When local content is forgotten before its remote withdrawal is acknowledged, the client SHALL retain only encrypted remote identifiers, expected revisions, removal barriers and retry state needed to complete that withdrawal. It SHALL explain pending remote availability, retry after connection and required sign-in return, and delete these records after acknowledgment. No source text or publication payload is retained for this purpose.

#### Scenario: Delete everything offline
- **WHEN** a user deletes all data while offline with published knowledge still present on the server
- **THEN** the local text and derived content are deleted, the first-run view reports pending remote removal, and reconnecting and signing in as the same user permits withdrawal without restoring the deleted knowledge

#### Scenario: Withdrawal records cannot be preserved
- **WHEN** Delete all cannot durably preserve the identities needed for pending remote withdrawal
- **THEN** it reports the failure and does not erase the only remaining withdrawal records or sign the user out

#### Scenario: Concurrent removal records
- **WHEN** two processes add pending remote removals to the same profile at the same time
- **THEN** neither process silently overwrites the other's committed record and a stale snapshot must be reloaded before retry

### Requirement: Forget Invalidates Recovery Copies
Forgetting data SHALL invalidate or purge its recoverable copies in local migration backups, staged revisions, compaction summaries and analysis caches. A later recovery operation MUST NOT restore content recorded as forgotten after that backup was made.

#### Scenario: Rollback after forgetting a Source
- **WHEN** a migration backup predates a confirmed Source forget operation
- **THEN** rollback cannot make that Source or its unsupported derived knowledge readable again

### Requirement: Migration Backup Includes Committed Writes
An encrypted migration backup SHALL include all commits preceding its migration and be consistent even when write-ahead logging is in use. When backing up by copying the main file, the client SHALL confirm that committed write-ahead-log content has been checkpointed. A failed or incomplete backup MUST leave the previous recovery copy and schema unchanged.

#### Scenario: A reader prevents checkpointing
- **WHEN** another database connection holds a read snapshot that prevents all committed changes from being checkpointed
- **THEN** migration reports that it cannot create a safe backup and leaves the database and its previous recovery copy unchanged
