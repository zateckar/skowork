# Design

## Context

Facts from the code on 2026-11-10 (main at 0c70d92):

- **Client timeouts.** The idle timeout is a fixed 60 s, and policy.llm.timeouts.* is never read. WinHTTP applies the idle timeout to the wait for response headers too, so it bounds the whole prefill.
- **Gateway: headers only after the first chunk.** The gateway commits headers only after the first upstream chunk. It waits up to 120 s for that chunk (`streamIdleTimeoutMs`).
- **Gateway: the 300 s timeout covers the whole stream.** `requestTimeoutMs` (300 s) is an AbortSignal passed to `fetch`, so it also aborts reading the body.
- **Upstream timing.** add-agent-runtime D0 measured prefill on the upstream at 8.6–9 s for 128k tokens (primary) and 48–53 s for 64k tokens (secondary), one request at a time. Under load, or on a cold cache, a long Thread passes 60 s before the first byte.
- **The system prompt changes every Run.** It is the base prompt, the Skills list, the Run context (with "Now: HH:MM"), the untrusted-data paragraph, and the summary wrapped with a new nonce on every rebuild.
- **Loaded tools reset every Run.** The tool set is rebuilt per Run from that Run's `tools_loaded` events.
- **Tool-call arguments are re-serialized.** Earlier assistant tool calls are resent as `arguments.dump()`, which reorders keys.
- **Summary requests are not streamed.** They use `stream = false` with the `small` role.
- **Retry starts a new Run.** `threads.retry` sends the failed Run's text as a new user message in a new Run. The failed Run's messages stay in the Thread context.

## Goals / Non-Goals

**Goals**
- A Thread that worked at Turn 1 keeps working at Turn 30.
- The time per Turn grows with the new content, not with the Thread's length.
- Retry recovers whenever the cause was transient.

**Non-Goals**
- Model fallback and reasoning_effort, which are in a separate change.
- Compaction policy, owned by improve-skowork-and-knowledge-control.

## Decisions

### D1. The gateway sends headers and keep-alives before the first token
For a streamed chat request the gateway sends `200` and the SSE headers as soon as it has picked a backend. It then writes an SSE comment line (`: keep-alive`) every `WORKBENCH_LLM_KEEPALIVE_MS` (default 15 s) until upstream data arrives, and also later whenever the upstream is idle for that long.

**Failover stays possible.** Keep-alives are not upstream bytes, so failover to another backend stays possible until the first upstream byte is forwarded. Backend Failover is reworded around "first upstream byte".

**Errors after the headers.** An error after the headers have been sent can no longer use the HTTP status. It is written as one `data: {"error":{"code":…,"message":…,"retry_after_s":…}}` event. The stream then ends without `[DONE]`. The client already parses an `error` object inside a stream chunk and maps it to a typed error.

**Alternatives considered**
- *Raise the client idle timeout to 5 minutes:* it hides a dead connection for 5 minutes and still fails on a cold 128k prefill under load.
- *Have the client poll a job id:* it adds a second protocol.
- *Send empty `data:` chunks instead of comments:* they risk confusing third-party OpenAI parsers. Comments are ignored by every SSE parser.

### D2. Stream time limits replace the total request timeout
The gateway has three limits:
- `WORKBENCH_LLM_FIRST_TOKEN_TIMEOUT_MS`, default 180 s: from the upstream request to the first upstream byte.
- `WORKBENCH_LLM_STREAM_IDLE_MS`, default 120 s: between upstream chunks.
- `WORKBENCH_LLM_STREAM_MAX_MS`, default 20 min: a backstop.

`requestTimeoutMs` keeps bounding only non-streamed requests. For a streamed request, `WORKBENCH_LLM_FIRST_TOKEN_TIMEOUT_MS` bounds the response headers and the first byte, and the body read is not tied to any request timeout.

**Alternatives considered**
- *Only raise `requestTimeoutMs`:* it still cuts long reasoning answers, and it does not separate "slow to start" from "stalled".

### D3. The client applies policy timeouts and retries TIMEOUT once
`start_agent` reads policy.llm.timeouts.connect_seconds and idle_seconds, and re-reads them when the policy changes. Defaults stay at 10 s and 60 s, which is safe now that keep-alives arrive every 15 s.

**TIMEOUT is retried once.** A TIMEOUT now means that the gateway waited its full first-token or idle limit. Three more attempts would occupy the backend for up to 12 more minutes with no better chance. RATE_LIMIT and MODEL_UNAVAILABLE keep 3 retries.

**Alternatives considered**
- *Keep 3 retries for TIMEOUT:* the user waits up to 12 minutes before seeing the error.
- *No retry:* a single slow request under a load spike would fail the Run.

### D4. A byte-stable prompt prefix per Thread
vLLM's automatic prefix caching reuses the computed KV blocks of an identical token prefix. The request is ordered from most to least stable:

1. **The system message (fixed per Thread until the next compaction):**
   - the base prompt;
   - the Skills list, sorted by name;
   - the stable Run context: user, time zone and UTC offset, folders;
   - the untrusted-data paragraph;
   - the summary, if any, wrapped with the nonce stored in its compaction record.
2. **Tool definitions:**
   - the core tools first, in a fixed order;
   - then the tools loaded in this Thread, in the order they were first loaded.
3. **The messages since the summary boundary.** Each message is resent exactly as stored. Tool-call arguments use the raw text the model produced; repaired arguments use the repaired text stored at repair time.
4. **The user message that starts a Run** carries a first line `Current time: Thursday 22 October 2026, 14:05 (UTC+02:00)`. The line is stored with the message, so it is identical when the message is resent in later Turns.

**Time-only changes.** A change of minute, day or time zone changes only the newest message. A change of folders, Skills or user rebuilds the system message, which is rare.

**Loaded tools.** Loaded tools stay for the Thread up to the existing tool-schema bound. When a newly loaded tool would pass the bound, the earliest-loaded tools are dropped. That one change invalidates the cache once.

**Effect.** For a Thread of N Turns, Turn N prefills only the new user message, plus the last answer and tool results that the previous request did not contain.

**Alternatives considered**
- *Keep the time in the system prompt but round it to the day:* this loses the time of day, which "remind me in two hours" needs, and still misses the cache at midnight.
- *Put the time in a trailing system message:* several chat templates reject or reorder a second system message.
- *Send all tools on every request:* this costs 20k or more tokens (add-agent-runtime D3).

### D5. Summary requests are streamed
The summarizer call uses `stream = true` and collects the text. It keeps the `small` role and its input caps from improve-skowork-and-knowledge-control. With keep-alives and streaming, a 52k-token summary input (secondary prefill of about 50 s, plus generation) no longer meets the client idle timeout.

**Alternatives considered**
- *Run the summary on `main`:* the primary model prefills about 10× faster, but this changes role semantics and budget accounting. It is left to the compaction change.

### D6. Retry continues the failed Run
`threads.retry` on a failed Run resumes the same Run instead of creating a new one:
- The Run returns to `queued`, and its existing user message is kept.
- Completed tool results are kept. Idempotent resume already guarantees that completed tool-call ids are not executed again.
- Nothing from the failed model call is stored as a conversation message. Its partial output is shown in the Activity log only.

**Lifecycle.** This needs one new transition, failed → queued, and only Retry may use it. A call that has a `tool_call` or `interrupt` event but no result may have partly run, for example when the client shut down during a tool. Retry records a failure result for such a call ("did not complete; not executed again") instead of running it again. A call with no event at all never started, so it runs normally.

**Other states.** A Run that failed before its first model call is resumed in the same way. Retrying a Run cancelled by the user keeps today's behaviour: it sends a new message. Retry is offered only on the latest Run of a Thread.

**Failed Runs in later context.** A failed Run that is never retried keeps its user message in the Thread context, so the model knows that the user asked. Its incomplete model output is not included.

**Alternatives considered**
- *Remove the failed Run from the context and send a new one:* this changes the stored history, and the user loses the record of the failure.

### D7. Cache and timing audit
The gateway records two values per chat request:
- `ttft_ms`: from the upstream request to the first upstream byte.
- `cached_tokens`: from `usage.prompt_tokens_details.cached_tokens`, or empty when the upstream does not report it.

Both values are stored in the gateway audit entry, which is how acceptance checks D4 against the real upstream. The admin console is not changed.

**Alternatives considered**
- *Measure on the client only:* the client cannot see cached tokens unless the gateway forwards usage details. Forwarding them is unchanged by this change.

## Risks / Trade-offs

- **[Risk] Prefix caching is off on the upstream vLLM, or LiteLLM strips `cached_tokens`.** → **Mitigation:** spike 1.1 checks it before anything else. Without caching, D1–D3 still remove the timeouts, but long Threads stay slow. The finding is recorded here, and the upstream owner is asked to enable `--enable-prefix-caching`.
- **[Risk] LiteLLM does not abort the vLLM request when the gateway disconnects, so abandoned attempts keep the GPU busy.** → **Mitigation:** spike 1.2 measures this. With TIMEOUT retried only once, at most one abandoned attempt overlaps its replacement.
- **[Risk] A proxy between the client and the gateway buffers SSE and swallows the keep-alives.** → **Mitigation:** the gateway already sets `X-Accel-Buffering: no` and `Cache-Control: no-cache`. The acceptance test runs through the dev-proxy path that real workstations use.
- **[Risk] The time on the user message makes the model mention the time unprompted.** → **Mitigation:** a fixed label "Current time:" on its own first line. A short eval checks five prompts for unprompted mentions.
- **[Risk] Thread-wide loaded tools widen what the model sees in later Runs.** → **Mitigation:** policy is still enforced on every call, and only permitted tools can be loaded. The tool-schema bound stays the same.
- **[Trade-off] Errors after the headers lose their HTTP status.** Clients must read the error event. The current client already does, and other consumers of the gateway are internal.

## Performance (baseline hardware)

All client-side changes run in string assembly the runtime already does, below 1 ms per request. The prefill saving is on the upstream. Using the D0 numbers, on a warm cache a Turn at 100k tokens of Thread prefills only its new content, typically 1–10k tokens. That puts the first token at 1–3 s on the primary model, where today the full Thread is prefilled every Turn: about 7 s idle, and above 60 s under load.

## Spike results (2026-11-10)

All runs went directly to the organization LLM API from `.env`, one request at a time, from the developer workstation. The model ids are `skoda-llm-primary` and `skoda-llm-secondary`; elsewhere this design uses the placeholder names from add-agent-runtime D0.

### 1.1 Prefix caching (`dev/scripts/llm-cache-spike.mjs`)
Prefix caching is on, and LiteLLM passes `usage.prompt_tokens_details.cached_tokens` through. Times are to the first token.

**Primary**

| Request | Time to first token | Cached tokens |
|---|---|---|
| Cold, 44.8k-token prompt | 2.9 s | 0 |
| Identical prompt | 0.6 s | 43,008 |
| Same prompt plus one Turn | 1.0 s | 43,008 |

A repeated probe at 29k tokens hit the cache every time, including after an extra Turn.

**Secondary**

| Request | Time to first token | Cached tokens |
|---|---|---|
| Cold, 61k-token prompt | 8.8 s | 0 |
| Identical prompt | 0.7 s | 60,000 |
| Same prompt plus one Turn | 20.9 s | 0 |

A repeated probe at 43k tokens missed once and then hit, with exactly the cached count of the earlier prompt. This is consistent with several replicas behind non-sticky routing: each replica keeps its own cache. The gateway cannot fix this. The upstream owner should be asked for prefix- or session-affinity routing for the secondary model. Until then, summary requests (D5) hit the cache only by chance.

### 1.2 Abort propagation
Before the abort, a short request took 0.27 s to the first token on the primary and 0.22 s on the secondary. The test aborted a 110k-token streamed request after 5 s and sent a short request right away. It then took 0.86 s on the primary and 0.23 s on the secondary. No long blocking was observed, so the abandoned request does not hold the backend for its full prefill. The extra 0.6 s on the primary may be the end of the prefill already under way.

### 1.3 Reproduction (`dev/scripts/llm-thread-spike.mjs`)
The script simulates a Thread that grows by about 6.7k tokens per Turn (a user message, a tool result of about 5k tokens and two short answers) on the primary model. It sends the requests directly, without the client.

**"Old" layout:** the time line is in the system prompt and changes every Turn.
- 0 cached tokens on every Turn.
- Time to the first token grew with the Thread: 0.9 s at 13k, 2.2 s at 33k and 4.3 s at 54k tokens.
- Turn 9, at 60k tokens, took 47.5 s, close to the client's 60 s idle timeout.
- The next request did not get a connection within 10 s. This matches the reported failure, and it gets worse under load.

**"New" layout (D4):**
- Every Turn reused the previous prefix, with cached tokens of 6,144, 12,288 … 86,016.
- Time to the first token stayed at 0.57–0.92 s through Turn 14, at 93k tokens.
- The run stopped at Turn 15 with an upstream 429: the spike's request rate hit the upstream's limit. It was not a timeout.

## Acceptance

- **Tests (2026-11-10):**
  - Control plane: `npm test`, 262 of 262 pass, including 11 new gateway stream tests.
  - Client: `win_tests`, 690 of 690 pass; `base_tests`, 46 pass. Six shell tests had expected the old product name "Workbench" where the UI says "SkoWork"; their assertions were updated in this change.
- **4.2 Unprompted time mentions:** 0 of 5. Five ordinary prompts (a thank-you note, an explanation, tips, a translation, a summary) went to the primary model with the "Current time:" line, and no answer mentioned the date or time.
- **4.1 End to end:** pending. It needs the desktop client signed in at the organization IdP against the local control plane. Spike 1.3 already covers the upstream half: with the new layout the time to the first token stayed below 1 s up to 93k tokens.
