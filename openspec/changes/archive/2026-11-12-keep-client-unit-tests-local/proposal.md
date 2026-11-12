## Why

The product owner requests that client unit tests run locally only. GitHub Actions currently builds and executes them on Windows in both CI and tagged releases, and runs the portable unit suite under Linux sanitizers.

## What Changes

- Disable client unit-test compilation and execution in CI and release builds, removing test-only display and mock-server setup and golden-test artifacts.
- Remove the portable sanitizer unit-test job and its publication dependencies; retain the separate parser fuzz job.
- Preserve local CMake test presets, sources and commands, plus control-plane and Graph mock tests, product build checks, signing, packaging and publication.
- Reconcile the foundation change's D9 with this decision.

## Capabilities

### New Capabilities
- `client/build-pipeline`: client pipeline checks and local unit-test execution.

### Modified Capabilities
None.

## Impact

GitHub Actions workflows and OpenSpec only. No client runtime behavior or local test configuration changes. The user explicitly authorizes implementation and a fresh pipeline run.
