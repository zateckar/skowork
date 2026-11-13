## Purpose

Define which client checks run in GitHub Actions and which unit-test suites remain available for local development.

## ADDED Requirements

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
The pipeline SHALL retain client product compilation, manifest and footprint checks, optional signing, packaging and publication. Separate parser fuzzing and control-plane and Graph mock tests SHALL remain automated. Publication dependencies SHALL reference existing jobs only.

#### Scenario: Publish a main build
- **WHEN** every remaining required CI job succeeds for a main push
- **THEN** the workflow publishes the control-plane image and desktop packages without requiring a client unit-test job
