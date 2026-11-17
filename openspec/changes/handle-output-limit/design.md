# Design

## Context

`AgentRuntime` sends each model request with `max_tokens` set to the role's `max_output_tokens` from the gateway's model list. The response's finish reason is already parsed (`ChatResponse::finish_reason`) and only used to decide whether a generated surface may activate. A response without tool calls ends the Run as succeeded regardless of the finish reason.

## Decisions

### D1: Continue, do not end

A response whose finish reason is `length` and that has no valid tool call is not a final answer. The runtime stores it like any assistant message (its text and reasoning stay visible), then makes the next model request with one extra, request-only user note:

> The previous reply stopped at the output limit of N tokens before it was complete. Continue from where it stopped. Keep reasoning short; split long content over several tool calls, for example several smaller files.

`files.write` replaces a whole file, so "several smaller files" is the concrete way to split a large app.

The note also carries the end of the cut-off reply's reasoning and text, at most 8000 characters, introduced as the model's own unfinished draft. Reasoning is otherwise never resent, so without it the model starts planning again from zero and is cut at the same point (evaluation run 2: the expenses app drafted `server.js` in its reasoning three times and failed). From the second continuation the note also asks the model to call a tool now with what it has.

The note is not stored in the Thread and not resent later. It is added to the request the same way image tool results are (request-only messages), so the Stable Prompt Prefix of later requests is unchanged.

### D2: Two continuations, then fail

Consecutive output-limit responses are counted per Run. Any response that ends normally (`stop` or `tool_calls`) resets the count. The third consecutive output-limit response without a valid tool call ends the Run as failed with error `{"code":"output_limit","message":"The model reached its output limit (N tokens) before it finished.","partial_output":true|false}`. Two continuations give the model room to recover from a long reasoning phase without letting a looping model spend the user's budget.

### D3: Cut-off tool calls

When a response stopped at the output limit, the last tool call may be incomplete. `validate_tool_calls` already records whether arguments needed repair. For a `length` response, a call that needed repair is marked invalid with "the call was cut off at the output limit; send shorter arguments, for example split the content over several calls". The existing corrective re-prompt then asks for a corrected call. Calls whose JSON was complete are valid as before, so complete calls before the cut-off still run.

When a `length` response has valid tool calls, the Run continues through the tool loop as usual; the tool results are the continuation, so no note is added and the count is not increased.

## Evaluation

The dev stack maps `main` to a model with a 4096-token output limit. Run 1 (before this change): all three Local app Runs "succeeded" empty after their first reply spent 4096 tokens on reasoning. Run 2 (D1 without the carried draft, D2, D3): the presentation and the reading list were built, opened and work in the browser. Each Run continued once or twice after the limit, split the app over several files, and recovered from a cut-off `files.write` through the corrective re-prompt. The expenses app failed with `output_limit` after drafting `server.js` in its reasoning three times, which led to carrying the draft (D1).

Run 3 (2026-11-17) used `skoda-llm-secondary` with its corrected output limit of 131,072 tokens and the vendor's sampling values (add-model-sampling-settings). The expenses app was built and works, and no reply reached the limit. This run did not exercise the continuation with the carried draft, so task 3.1 still needs a run on a role with a 4,096-token output limit.

## Risks

- A model that always reasons to the limit fails after three attempts. The error names the limit so an administrator can map a role with a larger output limit.
