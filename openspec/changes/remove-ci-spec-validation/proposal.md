# Proposal

## Why

The `ci` workflow runs `openspec validate --all --strict` as a job, and the control-plane image and the dev desktop packages wait for it. Several in-flight changes fail strict validation while they are being worked on, so the job fails and blocks publication of builds that are otherwise fine. Spec validation is a working rule for developers before each commit (AGENTS.md) and before each archive (openspec/config.yaml). It does not need to gate the product pipeline.

## What Changes

- Remove the OpenSpec validation job from the `ci` workflow. The control-plane image and the dev desktop packages no longer wait for it.
- client/build-pipeline Remaining Pipeline Checks no longer requires OpenSpec validation in the pipeline. Validation stays available locally, as parser fuzzing and the control-plane tests already are.

Out of scope:
- Fixing the in-flight changes that fail strict validation today.
- Changing the local rule to validate before every commit, or the archive rule.
- Any other workflow (release.yml and the optional package workflows have no spec validation).

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/build-pipeline`: Remaining Pipeline Checks drops OpenSpec validation from the pipeline.

## Impact

- **Roadmap:** proposed position 49 (Hardening), after add-manual-meeting-capture.
- **Depends on:** keep-client-unit-tests-local (archived), which defined Remaining Pipeline Checks.
- **Forward references:** none.
- **Code:** `.github/workflows/ci.yml` only.
