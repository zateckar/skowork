## ADDED Requirements

### Requirement: Pinned Speech Build Inputs
Speech packaging SHALL fetch a versioned build-input bundle from this repository using a source-controlled URL, byte length and SHA-256. The bundle SHALL preserve the model/runtime files pinned by speech-manifest.json and existing development recordings, including license notices. Changes to model bytes SHALL require a separate qualified model change. Build-input versions SHALL NOT be overwritten. Client speech installation remains owned by client/speech-models.

#### Scenario: Rebuild after an upstream archive is replaced
- **WHEN** CI or a developer fetches speech inputs after an upstream model archive changes
- **THEN** the fetcher downloads the pinned repository bundle and verifies its length and digest
- **AND** it verifies all 13 model/runtime files against the existing manifest before replacing local model files
- **AND** it preserves local test recordings and the existing vendored runtime DLLs

#### Scenario: Reject an invalid build input
- **WHEN** the bundle digest, size, required entries, unique paths or per-file hashes are invalid, or a vendored runtime file differs from the manifest
- **THEN** the fetcher fails before replacing any local model or recording

#### Scenario: Verify or import without a network download
- **WHEN** a developer requests verification only or supplies an offline archive
- **THEN** verification only checks the existing 13 assets without network access
- **AND** offline import applies the same pinned archive and per-file checks as a download

#### Scenario: Publish product speech artifacts
- **WHEN** CI builds the optional speech package from the pinned inputs
- **THEN** the package retains its existing model/runtime manifest and notices and excludes development recordings
