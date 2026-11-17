# Proposal

## Why

Relevant changes in mail and calendar can go unnoticed, and users have limited visibility into background work. A shutdown can also leave the outcome of a tool call uncertain, making a blind retry unsafe.

## What Changes

- Offer opt-in, source-linked triage of eligible mail and calendar changes into reviewable Threads, without automatically starting a Run or sending a message.
- Record effect-aware tool execution and offer safe continuation of shutdown-failed Runs without blindly repeating an uncertain external effect.
- Show background and Child Run progress, waiting, completion, cancellation and recovery status in the existing Thread and Inbox views.
- Preserve existing policy decisions, source exclusions, missed-schedule behavior, approval and interruption behavior; suppress Occurrence retries while an effect outcome is unknown.

## Capabilities

### New Capabilities

- `client/event-triage`: consent, source-event intake, relevance evaluation and reviewable suggestions.

### Modified Capabilities

- `client/agent-runtime`: effect-aware recovery and durable progress for Runs and Child Runs.
- `client/work-inbox`: presentation and control of triage and background work.
- `client/scheduler`: hold an Occurrence with an unknown effect for resolution; this capability is introduced by the prerequisite `add-scheduled-and-remote-tasks` change.
- `client/model-access`: let a background feature request one gateway attempt so its own request-spacing policy also governs retries.

## Impact

Roadmap: proposed after entry 47, dependent on `add-agent-runtime`, `add-policy-and-sandbox`, `add-work-inbox`, `add-scheduled-and-remote-tasks`, `add-mail-calendar`, `add-native-ui`, `fix-feed-live-updates`, `fix-tool-waits`, and `improve-skowork-and-knowledge-control`. These changes are still in flight; their applicable contracts and integration checks must be satisfied before dependent rollout. Archive `add-scheduled-and-remote-tasks` and `add-agent-runtime` first so this change can modify their `client/scheduler` and `client/model-access` requirements; rebase the full modified requirements on their actual archived versions before archive. Scheduler, mail sync, runtime, inbox and native shell code and tests are affected. `client/mail-calendar`, `client/desktop-host` and `client/tool-sandbox` are forward references owned by their changes.

Out of scope: new chat providers, automatic outgoing communication, autonomous Runs from suggestions, new server-side execution, an unrestricted shell, remote desktop handoff, shared bot memory, and changing missed-schedule behavior. No third-party Pots code is assumed.
