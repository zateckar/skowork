# Proposal

## Why

The Team expenses Local app evaluation on 2026-11-17 (add-model-sampling-settings, Evaluation) found two defects. Both damage files without telling anyone.

1. **Short spans of text were missing from streamed output.** About ten spans were missing from the files the model wrote, for example `padStart0')` for `padStart(2, '0')`. The model's own text was affected too: "write the server and the frontend in".
   - The gateway writes its keep-alive comment on a timer and pays no attention to where an upstream SSE event ends. When the upstream stalls in the middle of an event, the comment lands inside the event's `data:` line. The client cannot parse that event.
   - The client ignores data it cannot parse, so the delta is lost and the output keeps going without it.
2. **Parallel file edits interfered.** The runtime runs a turn's tool calls in parallel. Two `files.edit` calls on the same file both read the original, and the second write undid the first. Another pair failed with an access error, because one call was replacing the file while the other had it open.

## What Changes

- The gateway sends a keep-alive comment only between complete upstream events, never inside one.
- The client treats a streamed `data` payload it cannot parse as a malformed response, a retryable failure. It no longer skips the payload.
- The file tools serialize their work on the same file. A `files.edit` reads and writes under the lock, so an edit always applies to the latest content. A `files.read` waits while a write to the same file is in progress.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `control-plane/llm-gateway`: Keep-Alive Between Events (added).
- `client/model-access`: Malformed Stream Data (added).
- `client/core-tools`: File Write Tools (concurrent calls on one file).

## Impact

- Code: the gateway's `EventStream`, the client's `ChatAccumulator`, and `tools/files.cpp`.
- No API or data change.
- Existing gateway behaviour is otherwise unchanged. Keep-alives still go out before the first upstream byte and whenever the upstream is idle at an event boundary.
