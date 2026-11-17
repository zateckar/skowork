# Proposal

## Why

While a Run streams an answer, the user sees text appear but has no sense of how fast the model is working, and while the model has not produced its first token the user sees only a status line. A small, always-visible generation speed (tokens per second) under each answer shows that work is progressing and makes slow gateway periods recognisable without opening work details.

## What Changes

- Record, for every model call of a Run, when the request started, when its first output token arrived, when it finished and how many output tokens it produced.
- Under each answer of a top-level Run, show the generation speed in tokens per second: an estimate while the answer streams, the measured value after the Run's model calls finish.
- Before the first output token of a model call, show how long the Run has been waiting for the model.
- The speed line is visible by default. It is not a work detail; the Run's detailed token usage stays a work detail.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `client/agent-runtime`: a Run event per model call with its timing and output token count.
- `client/work-inbox`: the generation speed line under each answer.

## Impact

Roadmap: proposed after entry 47. Depends on `add-agent-runtime` (usage accounting in `client/model-access`, Run events), `add-work-inbox` and `fix-feed-live-updates` (live feed updates and the Work Details Visibility requirement). `fix-feed-live-updates` must be archived first; at archive, Work Details Visibility is rebased so its list of what the feed shows by default includes the generation speed line (design D3). Runtime, feed model, feed rendering and localized strings (en, cs, de) are affected. No control-plane or gateway change.

Out of scope: cost display, per-model-call breakdowns in the default view, time-to-first-token statistics, telemetry of speeds to the control plane, and changing which upstream model serves a role.
