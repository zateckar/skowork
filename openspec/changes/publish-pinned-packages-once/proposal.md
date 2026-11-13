# Proposal

## Why

Every push to main and every version tag rebuilds and uploads the speech package (1,096,195,563 bytes) and the app runtime package (100,261,283 bytes), although both are pinned and unchanged. A main push replaces the rolling dev pre-release, so each push uploads about 1.2 GB. Each version tag attaches the speech package to a new GitHub release.

## What Changes

- The speech package is built from its pin and published once as an immutable release asset. A workflow builds it when the pin changes or when started by hand, and never replaces an existing release.
- Main pushes and version tags no longer build, upload or attach the speech package. The dev pre-release and the product release carry the Skowork package and the app runtime package.
- The app runtime package is restored from a cache keyed by its pinned version and script. The upstream download runs only on a cache miss, and every fetched runtime is checked against its SHA-256 and Authenticode signature before packaging.
- Release notes link to the speech package release and its SHA-256 instead of attaching the file.

## Capabilities

- `client/build-pipeline`: Remaining Pipeline Checks (modified), Speech Package Publication and App Runtime Cache (added).

## Decisions amended in other changes

- `improve-skowork-and-knowledge-control` design, 2026-11-12: "The package stays our own single zip, built and published with each release like the Local apps runtime." This change supersedes that sentence. The speech package is published once per version and referenced from policy. This needs the user's confirmation.
- `pin-speech-build-assets` delta, scenario "Publish product speech artifacts": CI builds the package on every run. This changes to: the speech package workflow builds it when its pin changes.

## Out of scope

- Model bytes, the manifest, the client's verification and the build-input bundle (`speech-assets-v1`).
- The Skowork package and the control-plane image, which keep their current publication.
- Code signing.

## Impact

- CI: `ci.yml`, `release.yml`, a new `speech-package.yml`, and a runtime cache step.
- Operations: policy `speech.package.url` and `speech.package.sha256` point to the published speech package. Installed clients are unaffected.
- Risk: the speech package now depends on its own release for availability. The client still checks the digest, so a replaced file is refused.
