## Why

CI run 43285528704 built the client successfully but could not package speech: the upstream Parakeet archive was replaced on 2026-11-07 and no longer matches the pinned archive or model hashes. All 13 existing local model/runtime assets still match speech-manifest.json. Advancing those hashes would introduce an unqualified model change.

## What Changes

- Publish the verified existing speech assets and six existing development recordings as a versioned build-input release asset in this repository.
- Pin its URL, byte length and SHA-256 in source; fetch that bundle rather than mutable upstream archives.
- Retain all per-file manifest checks, verify the complete bundle before replacing local models, and support offline import of the exact pinned archive.
- Preserve local test recordings, vendored runtime DLLs, model versions and consumer speech-package layout.

## Capabilities

### Modified Capabilities
- `client/build-pipeline`: reproducible speech build inputs and failure-before-replacement integrity checks.

## Impact

Changes the developer/CI fetch script and adds build-input packaging and isolated regression checks. No client unit tests return to Actions. The product speech installer and policy continue to follow client/speech-models. Publication is authorized by the user's request to execute this proposal.
