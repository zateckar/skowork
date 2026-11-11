# Proposal

## Why

On 2026-11-10 the product owner reported that model requests often time out once a Thread has many Turns. A new Thread answers at once. Retry never recovers the Thread. An investigation of the code found these causes, which add up:

- **No bytes before the first token.** The gateway sends nothing to the client until the upstream returns its first chunk, and vLLM returns it only after it has read the whole prompt. The client gives up after 60 seconds without data, which is shorter than the gateway's own 120-second wait for the first chunk. So the client always gives up first, and the gateway's failover to another backend before the first byte never takes effect.
- **No prompt reuse between Turns.** The Run context puts the current time near the top of the system prompt, and it changes with every Run. Two more things change the prompt per Run: the compaction summary is wrapped in a new random token each time, and loaded tools are dropped at the end of each Run. vLLM's prefix cache therefore cannot reuse the earlier Turns, and every Turn reads the whole Thread again. The longer the Thread, the longer that takes.
- **The summary request cannot finish.** The summarizer call of a compaction is not streamed and runs on the slower `small` model. Once a Thread reaches the compaction trigger, every Turn first tries to compact, times out up to four times, keeps the full history and then sends it anyway.
- **Retry grows the request.** Retry starts a new Run with the same user message, while the failed Run stays in the conversation. Each retry is longer than the last, and none can reuse the earlier prompt.
- **Policy timeouts are not applied.** policy.llm.timeouts.* is specified but never read by the client. The gateway's 300-second request timeout also cuts long streamed answers.

## What Changes

- The gateway sends response headers at once and a keep-alive comment while it waits for the first token. It reports errors that occur after the headers as an error event in the stream. A stream is limited by the time to the first upstream token and by the idle time between tokens, not by a total duration.
- The client applies policy.llm.timeouts.*. It retries TIMEOUT once instead of three times. It requests every chat call as a stream, including summary requests.
- The runtime keeps the prompt prefix byte-identical from Turn to Turn in a Thread:
  - The system prompt holds only what does not change within a Thread.
  - The current time travels with the user message that starts the Run.
  - The compaction summary keeps its boundary token until the next compaction.
  - Tools loaded in a Thread stay loaded in load order.
  - Earlier tool-call arguments are resent exactly as the model wrote them.
- Retrying a failed Run continues that Run. It adds no second user message, and no output of a failed model call is sent to the model again.
- The gateway records the time to the first token and the number of prompt tokens served from the cache, so cache reuse can be checked.

Out of scope:
- Falling back to another model when the primary times out, and a configurable reasoning_effort per model. Both are in a separate change.
- Running the summary on another Model role, and changing the compaction trigger or the Working Context Budget (improve-skowork-and-knowledge-control).
- UI changes: feed live updates, freezes, and text selection.
- Pruning tool results outside compaction. It would change the prompt prefix on every Turn.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/model-access`: Streaming Responses, Retries With Backoff and Request Timeouts are modified. Stream Errors is added.
- `client/agent-runtime`: Run Lifecycle States (failed → queued for Retry), Run Context, Run Context Stability and Lazy Tool Loading are modified. Stable Prompt Prefix and Run Retry are added.
- `control-plane/llm-gateway`: Backend Failover is modified. Stream Keep-Alive, Stream Time Limits and Prompt Cache Audit are added.

## Impact

- **Roadmap:** 37 [Hardening]. It depends on add-agent-runtime and add-llm-connections and is archived after them. It is compatible with improve-skowork-and-knowledge-control, which owns the compaction trigger and budget. This change only requires that the summary request is streamed.
- **Code:**
  - client/src/model: the timeout configuration, the retry count and handling of stream error events.
  - client/src/runtime: system prompt assembly, the Run context, the compaction nonce, the tool set per Thread, raw tool-call arguments and the resume of a failed Run.
  - client/src/app: policy timeouts and the Run context wiring.
  - client/src/inbox: Retry.
  - control-plane/src/gateway: keep-alive, time limits and audit fields.
  - control-plane/src/config.ts.
- **Data:**
  - The client stores the raw arguments of each tool call and the time line of each Run-starting message in the existing event log.
  - The Thread stores the loaded tool names and the summary's boundary token.
  - Two new columns in the gateway audit: time to first token and cached prompt tokens.
- **Compatibility:** an older client reads the keep-alive comments as SSE comments and ignores them. It maps a stream error event to a typed error, because the stream parser already handles error objects.
