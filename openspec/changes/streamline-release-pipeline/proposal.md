## Why

Publishing waits on unrelated tests and repeatedly packages optional runtime assets. The speech release existence check fails when the release is absent.

## What Changes

- Build and publish the client and control-plane image with essential product checks. Keep test suites available locally.
- Publish the Node runtime and speech package independently only when their inputs change or manually requested; retain immutable versioned assets.
- Distinguish missing releases from API errors; link optional packages from product releases.

## Capabilities

### Modified Capabilities
- `client/build-pipeline`: product publication and optional package lifecycle.

## Impact

Changes GitHub Actions release workflows and optional package packaging. No client or model behavior changes.
