# Proposal

## Why

When a Run reaches one of its limits (model_calls, tool_calls or wall_minutes), the runtime fails it with a reason such as "tool call limit reached". Whatever the agent found or did up to then is in tool results the user does not see by default, and there is no answer. The user sees a failure, and an Unattended run reports nothing useful. Long, legitimate tasks such as reviewing many files or building a Local app reach the unattended limit of 80 tool calls.

Other agent harnesses end differently. One makes a last request with the tools left out, so a run always ends with text. Another returns a subagent's partial result marked as partial when it reaches its turn limit.

## What Changes

- At a limit, the runtime executes no further tool calls and makes one final model call without tool definitions. It asks the model to say what it did, what it found and what remains.
- The Run then ends succeeded with that answer and records the limit it reached. If the final call fails, the Run fails with the limit as its reason, as today.
- A Child Run that ends at a limit gives its parent the answer marked as stopped at that limit.
- The Thread feed shows a note under such an answer that names the limit and says that a reply continues the work.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `client/agent-runtime`: Run Limits (modified), Final Answer At A Limit (added), Child Run At A Limit (added).
- `client/work-inbox`: Run Limit Note (added).

## Impact

- Roadmap: Hardening, after improve-transcript-language. Depends on add-agent-runtime (Run Limits, Subagents) and add-work-inbox (Thread feed).
- Code: the limit paths of the runtime loop (`client/src/runtime/runtime.cpp`), the feed rendering of a finished Run, and the UI strings in en, cs and de.
- Notifications: such a Run raises the "completed" notification of Desktop Notifications.

Out of scope: the Child Run Time Limit of `fix-tool-waits` (a child that waited too long for the user ends as failed, as specified there); raising the limits; cancellation by the user, which still ends the Run cancelled.
