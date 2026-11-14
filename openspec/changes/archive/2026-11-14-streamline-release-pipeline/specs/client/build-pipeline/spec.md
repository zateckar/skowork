## MODIFIED Requirements

### Requirement: Remaining Pipeline Checks
The pipeline SHALL compile the client product, check manifests and footprint, optionally sign it, build the control-plane image, and publish both products. OpenSpec SHALL be validated. Parser fuzzing, control-plane and Graph mock tests SHALL remain available locally and SHALL NOT gate publication.

#### Scenario: Publish a main build
- **WHEN** product builds and OpenSpec validation succeed for a main push
- **THEN** the workflow publishes the control-plane image and desktop package, with links to independently published optional packages

## ADDED Requirements

### Requirement: Independent Optional Runtime Publication
The Node runtime package SHALL be published once per content-derived input identity, independently of client and image releases. It SHALL contain the verified pinned runtime, license and app host. Existing releases SHALL NOT be replaced.

#### Scenario: Runtime inputs unchanged
- **WHEN** a product build runs without a runtime input change
- **THEN** it does not build or upload a runtime package and links the existing runtime release

#### Scenario: Runtime inputs change
- **WHEN** the pinned fetch script, app host or runtime packaging script changes on main or its workflow is dispatched
- **THEN** the workflow verifies the pinned runtime and publishes a new content-identified package if absent

#### Scenario: Release lookup fails
- **WHEN** the release API returns an error other than not found or an existing release lacks required assets
- **THEN** the optional package workflow fails without replacing it
