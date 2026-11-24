# Design

## Context

Checked in the code and in the reporter's local data on 2026-11-24 (client e8a9812 plus uncommitted work).

**Estimate and trigger.** The indicator and the trigger use the same numbers.
- The indicator reads `threads.context` (`client/src/inbox/inbox_service.cpp`), which calls `AgentRuntime::context_usage` (`client/src/runtime/runtime.cpp`). It estimates the Thread's messages, summary, configured system prompt and the last request's tool definitions, applies the usage calibration of the main role, and divides by `working_context_budget(window, output_limit)`.
- The trigger is `maybe_compact` in the same file. It estimates the full request (messages and tool definitions) with the same calibration and compares it with the `trigger` of the same `working_context_budget`. Its estimate is at least the indicator's, because the request's system prompt is a superset of the configured one.

**When the trigger runs.** Only in `model_turn`, before a model request. When the model answers without tool calls, the Run ends at once. Nothing checks the trigger after the answer is added, and nothing checks it while the Thread is idle. `observe_usage` records the provider's prompt tokens of the last request after it returned, which can raise the calibrated estimate the indicator shows.

**Failures.** A failed compaction is recorded in the Activity log ("Compaction failed (...)"). Only `nothing_to_compact` is silent, and only when the Thread has no more than `compaction_keep_messages` messages after the last checkpoint.

**Reporter's data.** The local app.db (read with a probe built from the client's storage library) has no compaction record at all and no "Compaction failed" Activity entry. Its longest Thread holds 26 messages, about 89,000 estimated tokens without the system prompt and tool definitions. Its last Run made 6 requests with 312,281 prompt tokens in total. Its last request was therefore about 93,000 tokens, below the trigger of the reporter's model, so no compaction was attempted. The Run's final answer and the calibration then left the Thread at or above the trigger, where it stays until the next message.

**Second cause of "~100%".** Between 14:13 and 15:07 UTC on 2026-11-24 the control plane refused the client (HTTP 403), so the model roles were not loaded (`App::refresh_model_roles` runs only when the channel connects). Without role limits, `context_window` falls back to 32,768 tokens and the input budget to 24,576 (Model-Aware Budget Refresh). Any Thread above that shows "Context ~100%". No Run can start while disconnected, so no compaction can run either. This is out of scope here (follow-up below).

## Decisions

**D1. Check the trigger after the final answer, inside the Run.** Before a top-level Run ends as succeeded (a final answer, or the Final Answer At A Limit), the runtime builds the next request's view: the main role, `conv_` (which already holds the answer), the Run's current tool set and the main role's output limit. It passes it to `maybe_compact`. So the check, the calibration, the two-pass limit, the target and the Activity entries are the existing ones.
- Inside the Run, because the Run still holds the Thread. No other Run of the Thread can start and manual compaction is refused, so the checkpoint cannot race another writer. The compaction's usage is counted to the Run that caused it.
- Not in the idle Thread (for example a timer), because that would need its own locking, cancellation and usage owner.
- Not at a lower threshold (such as the indicator's 75% warning), because the trigger and target belong to Working Context Budget.

**D2. A failure does not change the Run.** The answer is already stored. `compact_now` records the failure in the Activity log and returns; the Run then ends as succeeded. The next request runs the usual check again.

**D3. Child Runs are excluded.** A child Run's history is not continued after it ends, so compacting it is wasted work.

## Risks

- A Run that ends above the trigger takes one summarizer request longer to end. The next Run's first request is shorter by the same request.
- A cancellation during this compaction stops the compaction (nothing is committed) and the Run still ends as succeeded, because its answer is complete. A runtime shutdown during it leaves the Run to the usual shutdown handling.

## Follow-up

- Context indicator without model limits: show that the budget is not known yet instead of a percentage of the 32,768-token fallback, and reload the roles when the model gateway answers again. Belongs to client/work-inbox and Model-Aware Budget Refresh.

## Acceptance

- 2026-11-24, before the change: `RuntimeCompaction.FinalAnswerOverTheTriggerIsCompactedBeforeTheRunEnds` failed (usage 7,861 against a trigger of 7,027, no compaction record), and `FailedCompactionAfterTheFinalAnswerKeepsTheRunsSuccess` failed (no "Compaction failed" entry). In both, the fourth Run's request was below the trigger, as required.
- After the change, both pass. `RuntimeCompaction.ProviderUsageAboveTheEstimateTriggersCompactionAndRefusalEarlier` asserted the old behaviour: no compaction after the Run that first reported the 1.5 ratio, although that ratio leaves the idle Thread above the trigger. It now expects that single compaction at the end of that Run, recorded under that Run.
- `win_tests --gtest_filter=Runtime*:StablePromptPrefix*:Inbox*:Shell*`: 324 tests, 318 passed, 6 skipped (they need a live gateway or environment), 0 failed. The release build succeeded.
- `openspec validate --all --strict` was not run: Node is not installed on this machine.
