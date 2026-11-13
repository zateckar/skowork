## MODIFIED Requirements

### Requirement: Remaining Pipeline Checks
The pipeline SHALL retain client product compilation, manifest and footprint checks, optional signing, packaging and publication. Separate parser fuzzing and control-plane and Graph mock tests SHALL remain automated. Publication dependencies SHALL reference existing jobs only. A main push and a version tag SHALL publish the Skowork package and the app runtime package; the speech package is published only by its own workflow (Speech Package Publication).

#### Scenario: Publish a main build
- **WHEN** every remaining required CI job succeeds for a main push
- **THEN** the workflow publishes the control-plane image and the Skowork and app runtime packages, without requiring a client unit-test job and without building or uploading the speech package

## ADDED Requirements

### Requirement: Speech Package Publication
The speech package SHALL be built only by its own workflow, from the pin in `client/tools/speech-package.json`, and verified against that pin before publication. Main pushes and version tags SHALL NOT build, upload or attach it. An existing speech package release SHALL NOT be replaced.

#### Scenario: Main push with unchanged speech pin
- **WHEN** a main push runs and `client/tools/speech-package.json` is unchanged
- **THEN** no speech package is built or uploaded

#### Scenario: Speech pin changed
- **WHEN** `client/tools/speech-package.json` changes on main or the speech package workflow is started by hand
- **THEN** the workflow builds the package from the pinned build inputs, checks its size and SHA-256 against the pin, and creates the release `speech-package-v<version>` once

#### Scenario: Speech release already exists
- **WHEN** the release for the pinned speech version already exists
- **THEN** the workflow stops without changing that release

### Requirement: App Runtime Cache
The app runtime package SHALL be restored from a cache keyed on its pinned fetch script, and fetched from upstream only on a cache miss. Its SHA-256 and Authenticode signature SHALL be checked before packaging.

#### Scenario: Runtime cache hit
- **WHEN** a build runs and the cache holds a runtime for the current fetch script
- **THEN** the workflow restores it without downloading from upstream

#### Scenario: Runtime cache miss
- **WHEN** no cached runtime matches the current fetch script
- **THEN** the workflow downloads the pinned Node.js runtime, checks its SHA-256 and Authenticode signature, and only then packages it
