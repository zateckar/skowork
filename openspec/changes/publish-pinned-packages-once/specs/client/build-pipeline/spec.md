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
