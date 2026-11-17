# Design

## Decisions

### D1: Keep-alives only at event boundaries

`EventStream` tracks whether the bytes forwarded so far end with a blank line, which marks the end of an SSE event (`\n\n` or `\r\n\r\n`). It also counts the first write as a boundary. The keep-alive timer writes its comment only at a boundary; otherwise it waits for the next interval.

The upstream idle limit (Stream Time Limits) still ends a stalled stream. Inside an event, the client's idle timeout can therefore fire before the gateway's. That is an accepted cost: an upstream that stalls mid-event for longer than the client's idle timeout is broken anyway.

### D2: Unparseable data fails the response

The client used to treat a `data` payload that was not JSON as a keep-alive or garbage, and ignore it. Keep-alives are SSE comments and never reach the accumulator. Every OpenAI-compatible server sends only JSON and `[DONE]`. A payload that does not parse is therefore a damaged event, and dropping it silently corrupts output.

The accumulator now ends the stream with a retryable `MODEL_UNAVAILABLE` failure. Its reason is "malformed stream event (N bytes)". The content is left out because it is model output, which does not belong in logs. The failure is retried like any other retryable model failure, and `partial_output` is set when deltas came before the damaged event.

### D3: One lock per file

The file tools serialize on the target path. A process-wide registry holds one mutex per path; the key is the absolute path, lower-cased.
- `files.write` and `files.edit` hold the lock for their whole read-modify-write.
- `files.read` holds it while it reads.

The lock does not cover other processes, `shell.run`, or two different spellings of the same file, such as an 8.3 name or a link. The parallel-call case the evaluation hit uses the same spelling every time.

A per-path lock in the tools is preferred over serializing all write tools in the runtime. Edits to different files keep running in parallel, and edits from two Runs are covered too.

## Risks

- **A server that sends non-JSON data lines now fails.** None of the supported servers does. The failure reason names the malformed event, so such a server is easy to identify.
