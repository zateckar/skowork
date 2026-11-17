# Proposal

## Why

A model call can stop because it reached the role's output limit (finish reason `length`). Today the runtime treats such a response like a finished one: when it carries no tool call, the Run ends as succeeded, even when the model spent the whole limit on reasoning and the answer is empty. A real-gateway evaluation on 2026-11-17 hit this on all three Local app requests: the `main` role offered 4096 output tokens, the model reasoned through all of them, and each Run "succeeded" with no answer, no tool call and no error. The user sees a finished Thread with nothing in it.

A response cut at the limit can also end inside a tool call. Argument repair can turn the cut-off JSON into valid arguments, so a `files.write` could run with a truncated file.

## What Changes

- A response that stopped at the output limit never ends a Run as succeeded. The runtime asks the model to continue, with a short note that it reached the limit and should keep its reasoning short and split large content over several tool calls. It does so at most twice in a row; then the Run fails with the error code `output_limit` and a readable reason, keeping any partial answer.
- A tool call in a response that stopped at the output limit runs only when its arguments were complete without repair. A call that needed repair is invalid ("cut off at the output limit") and goes through the existing corrective re-prompt.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `client/agent-runtime`: Output Limit Continuation.
- `client/model-access`: Tool Calls Cut Off At The Output Limit.

## Impact

Depends on `add-agent-runtime` (model loop, Run end states, error events) and its `client/model-access` Tool Call Validation And Repair. Runtime and model client only; no control-plane, gateway or UI change. The failed Run shows its reason through the existing error rendering.

Out of scope: raising a role's output limit (that is the administrator's role mapping and connection settings), and splitting the continuation note into localized strings (it is model-facing text).
