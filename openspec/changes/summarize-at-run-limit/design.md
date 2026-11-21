# Design

## Context

The runtime checks model_calls and wall_minutes before each model call, and tool_calls when it admits each call of a batch. At the tool-call limit it runs the calls admitted so far, closes the rest of the batch as unexecuted with "tool call limit reached", and ends the Run failed with that reason. The other limits end the Run failed in the same way.

## Decisions

### D1: One final model call without tools

At a limit, the runtime closes every unexecuted call of the current batch with a result stating that it was not executed because of the limit, so each call in the conversation has its result. A call that ran with an unknown outcome is told as unknown instead (it may have taken effect), and the Run's open Approvals and Questions are resolved as cancelled with the limit as reason, since nothing can act on them after the final answer. Then it appends one user-role instruction and makes one model call without tool definitions:

> The Run has reached its <limit> and cannot use tools any more. Answer the user now: what you did, what you found, and what remains to do. Do not say the task is finished unless it is.

The answer is stored as the Run's answer, like any final answer. The instruction is not shown in the feed.

Alternatives considered:
- Fail as today: loses the work, and the user has to ask again from the start.
- Ask the user to raise the limit with an Action item: blocks Unattended runs, and the limits are policy, not a user choice.
- Let the model continue with a small extra budget: only moves the problem.

### D2: The Run ends succeeded, with the limit recorded

The Run ends succeeded, because it produced an answer. It records a Run event `run.limit` with the limit's name, and the Activity log names it. The feed's note (Run Limit Note) comes from that Run event, not from the model's text, so it is shown even when the answer does not mention the limit.

Alternatives considered:
- End failed and still show the answer: the error banner and the "failed" notification suggest that nothing useful happened.
- A new Run state "stopped": the glossary's Run states are used across the client, the scheduler and telemetry; a recorded reason is enough.

### D3: The final call is outside the limits

The final call is not blocked by model_calls (it is recorded in the Run's usage and model call count like any call), and it is made even when wall_minutes has passed. It is bounded by the ordinary request timeouts of client/model-access, and compaction applies to it as to any request. It is made at most once per Run.

### D4: A Child Run's parent knows the result is partial

For a Child Run, the result its parent receives starts with "The subagent stopped at its <limit>; this is its account of the work so far:", followed by the answer. The parent can then decide to continue, delegate the rest or report it.

### D5: Unattended runs

The same rule applies. Unattended limits are lower (30 model calls, 80 tool calls, 30 minutes), so this is where it matters most: a scheduled task now delivers a partial result instead of a failure. The Run event and Activity log keep the limit visible in the Trigger's run history.

## Risks

- [The model claims the task is complete] -> The instruction forbids it, and the note under the answer comes from the client, not the model.
- [A loop that keeps hitting the limit now looks like success] -> The Run event `run.limit` is recorded and named in the Activity log, and the note is always shown, so repeated limits stay visible in the Thread and in a Trigger's run history.
- [The final call fails or gives no text] -> The Run fails with the limit as its reason, as today. A user who presses Stop during the final call cancels the Run as usual.
- [A Child Run Time Limit (fix-tool-waits) is reached] -> That limit keeps ending the child failed: it means the child waited for the user too long, and there is nothing new to summarise.
