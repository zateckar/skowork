# client/build-pipeline Specification

## Purpose
Define which client checks run in GitHub Actions and which unit-test suites remain available for local development.

## Requirements

### Requirement: Local Client Unit Tests
Client unit tests SHALL be built and executed locally only. GitHub Actions CI and tagged-release workflows SHALL configure client product builds with SKOWORK_TESTS=OFF and SHALL NOT execute client CTest suites. Local CMake presets, test sources and test commands SHALL remain available.

#### Scenario: GitHub Actions builds the client
- **WHEN** a pull request, main push or version tag triggers a client product build
- **THEN** the workflow disables client unit-test targets and does not run Windows or portable Linux client unit suites
- **AND** test-only display setup, mock-server installation and golden-test artifact collection are absent

#### Scenario: Developer runs client tests locally
- **WHEN** a developer uses the existing local debug, release or sanitizer test preset
- **THEN** client unit tests remain available with their existing test sources and commands

### Requirement: Remaining Pipeline Checks
The pipeline SHALL compile the client product, check manifests and footprint, optionally sign it, build the control-plane image, and publish both products. OpenSpec SHALL be validated. Parser fuzzing, control-plane and Graph mock tests SHALL remain available locally and SHALL NOT gate publication.

#### Scenario: Publish a main build
- **WHEN** product builds and OpenSpec validation succeed for a main push
- **THEN** the workflow publishes the control-plane image and desktop package, with links to independently published optional packages

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
