# Design

## Context

Builds on add-client-foundation (see its design.md, D3 for the process model and D7 for tokens). The on-premise LLM is the organization LLM API, an OpenAI-compatible LiteLLM proxy over vLLM (facts in D0). Tool calling and vision work, but tool-call quality on long multi-step tasks and the usable context length are not yet measured. The runtime therefore has to be robust to imperfect tool calls and smaller context windows than frontier models have. All agent logic runs in the core process (`Skowork.exe`). Most of the time is spent waiting on the network.

## Goals / Non-Goals

**Goals:**
- A deterministic, persisted Run state machine that survives restarts and resumes interrupts exactly once.
- A small per-turn token footprint, through lazy tool loading, compaction and attachment offloading, so mid-size on-premise models work well.
- One gateway in front of every model, so keys, quotas and audit are central.

**Non-Goals:**
- Responses API, assistants or server-side threads. Chat Completions is the only wire contract.
- Running models locally. Agent reasoning, embeddings and reranking all use the organization LLM API through the gateway. The only local ML is speech-to-text (client/meeting-capture).
- Prompt engineering for specific skills.

## Decisions

### D0. Upstream LLM (verified 2026-10-21)
The organization LLM API was probed directly on 2026-10-21. It is a LiteLLM proxy in front of vLLM 0.31.0, authenticated with a Bearer API key. Its address and key are in the developer's `.env` (OPENAI_BASE_URL, OPENAI_KEY; see D8 for how the gateway reads them in development) and, in deployment, in the gateway's secret store only.

| Upstream model | Kind | Verified | Default role |
|---|---|---|---|
| acme-llm-primary | chat, reasoning model (`reasoning_content`, also streamed) | tool calls, vision, streaming | `main`, `vision` |
| acme-llm-secondary | chat, no reasoning | tool calls, streaming | `small` |
| acme-llm-embedding | embeddings, `/embeddings` | 4096-dimension vectors | `embed` |
| acme-llm-reranker | rerank, Cohere-style `/rerank` | `relevance_score` per document | `rerank` |
| acme-llm-image | image generation | vision chat returns 500 | none (out of scope) |

- **Streaming** ends with `data: [DONE]`, which is the terminal marker that truncated-stream detection checks for.
- **Not reported by the upstream:** `/model/info` returns 404, so context window, maximum output and concurrency are unknown. The gateway takes them from its server settings, sized by spike 1.2 below and, for concurrency, by spike 1.3.
- **Usable context length (spike 1.2, measured 2026-10-22 with `dev/scripts/context-spike.mjs`):** vLLM declares `max_model_len` 1,048,576 for acme-llm-primary and 262,144 for acme-llm-secondary (from the error to an oversized `max_tokens`). A needle-in-haystack probe (a code at 10%, 50% and 90% depth, temperature 0, one request at a time) found the needle in every case:

  | Prompt tokens | primary: found / latency | secondary: found / latency |
  |---|---|---|
  | 8k | 3/3, 1.3 s | 3/3, 2–6 s |
  | 32k | 3/3, 2.4–2.8 s | 3/3, 10–23 s |
  | 64k | 3/3, 4.1–5.0 s | 3/3, 48–53 s |
  | 128k | 3/3, 8.6–9.0 s | 3/3, 71–107 s |

  - **Server settings:** `context_window` is 131072 for acme-llm-primary (the largest size measured, not the declared 1M) and 65536 for acme-llm-secondary, where latency rather than recall is the limit. Simple retrieval is an upper bound; multi-step tool use over long context is what the eval suite (task 1.4) measures.
  - **Compaction threshold:** 85% (D4) is confirmed. Recall did not degrade toward the end of the window, so there is no reason to compact earlier. The client falls back to 32768 only when the model list reports no window.
- **Reasoning:** the primary model returns reasoning separately from the answer. The client keeps it out of later turns (client/model-access, Reasoning Output), which saves context.
- **Alternatives considered for the role mapping:**
  - `main` → acme-llm-secondary: lower latency, but no reasoning, and multi-step tool use is unproven. The Phase-0 eval (task 1.4) decides per Group; changing it is gateway configuration only.
  - A separate vision model: none is available (acme-llm-image fails vision chat), and the primary model handles images.

### D1. Runtime structure (C++20)
- **One thread per executing Run.** Each executing Run is driven by its own thread, at most policy.llm.max_concurrent_runs at a time. Interrupted and queued Runs have no thread. Model I/O uses synchronous WinHTTP. Cancellation closes the request handle from another thread through a `CancelToken`, which aborts the blocking call. A tool batch runs on a small pool of up to policy.llm.max_parallel_tools threads that exists only for that batch. A subagent (D5) runs on the pool thread of the parent's `delegate` call, so it holds no Run slot of its own.
  - **Alternative considered:** C++20 coroutines over asynchronous WinHTTP. Rejected for the MVP. A handful of threads costs little at three concurrent Runs, and blocking code is much easier to test and to reason about under cancellation. We can revisit this if the slot count grows by an order of magnitude.
- **Shared database writer.** A `RunStore` persists every state transition and message in `app.db` before emitting the UI event.
- **Alternative considered:** a LangGraph-style graph engine. Rejected because it is more machinery than a single loop needs, and pizza-bot's checkpoint growth problem (pizza-bot issue #58) shows the cost of snapshotting whole graph state. We store an append-only event log per Run instead (message, tool_call, tool_result, interrupt, resolution, state), which also gives idempotent resume: completed tool-call ids are never re-executed.

### D1a. JSON library
- **Choice (2026-10-22):** nlohmann/json 3.12.0 (MIT, one header, continuously fuzzed in OSS-Fuzz), vendored per add-client-foundation D2a. It is used by every client module, with parse errors caught at module boundaries (`parse(..., allow_exceptions=false)` for untrusted input).
- **Alternatives considered:** simdjson (fast, but read-only, so a second library would be needed for writing); RapidJSON (no release since 2016); a hand-written parser (an avoidable memory-safety risk for untrusted model output).

### D2. Hooks
- **Interface:** an in-process interface `IHook` with the eight hook points named in the spec. Handlers are registered at startup by subsystems: policy, telemetry, knowledge memory flush, and skills.
- **Mandatory policy handler:** the policy handler is registered first and is non-removable. The registry refuses a build without it, and a unit test enforces this.
- **Return types:** `BeforeToolCall` returns `Allow | Deny(reason) | Rewrite(args) | RequireApproval(reason)`.
- **No third-party hooks in the MVP.** Skills may contribute prompt fragments, but not code hooks.

### D3. Lazy tool loading
- **Per-turn tool set:** only the core tools are sent each turn: `find_tools`, `ask_user`, and the tools pinned by the active skill. This stays at roughly 2–4k tokens.
- **Discovery:** `find_tools` runs BM25 over the descriptions of permitted tools, using the same FTS5 engine as Thread search. The model selects up to N tools, which stay loaded for the rest of the Run.
- **Alternative considered:** sending every schema on every call, which is what Hermes does. That can cost 20k or more tokens per turn and degrades smaller models.

### D4. Compaction
- **Trigger:** compaction starts at 85% of the role's context window.
- **Order of operations:**
  1. `BeforeCompaction` runs. The knowledge memory flush (change 12) persists durable facts.
  2. The `small` role summarises the turns before the most recent K.
  3. The system prompt, the latest user message, pending tool calls and the summary are kept.
- **Storage:** the full history stays in `app.db`. Only the model's view is compacted.
- **Record:** each compaction is a row in `thread_compactions`. The row holds the summary, the boundary as the first kept message (Run id and event sequence), the carried messages, whether it was automatic or manual, and the token estimates before and after. The model's view of a Thread is the latest summary, then the carried messages, then every message from the boundary on. The view is rebuilt from these rows. That covers a resumed Run and a new Run in the Thread just like the Run that compacted, so the Thread is continued compacted after a restart. These rows are the compaction entries of the Thread's Activity log, and an automatic compaction also adds an `activity` entry to the Run's log.
- **Kept tail:** K is the 6 most recent messages (`compaction_keep_messages`). The boundary never falls on a tool result, so a tool call and its result stay together. It never falls after an assistant message whose tool calls are still unanswered. If the latest user message falls before the boundary, it is carried, so a long Run keeps its task.
- **Budget:** the request size is estimated at about four characters per token, over the messages and the tool definitions. The context window comes from the role's entry in the gateway model list. When the gateway reports none, the default is 32768 tokens. If the model still answers CONTEXT_LENGTH, the runtime forces one compaction and retries the turn once.
- **Summariser input:** the previous summary and the messages before the boundary, each cut to 2,000 characters. The whole input is capped at half the `small` role's window. The summariser call of an automatic compaction counts as a model call of the Run, and its usage is added to the Run's usage. A failed summary leaves the conversation as it is: an automatic compaction records an `activity` entry and continues, and a manual one reports the error.
- **Manual:** "Compact Thread" runs the same procedure on the caller's thread. It is refused with `thread_busy` while a Run in the Thread is running; an interrupted Run does not block it.
- **Subagents** are not compacted. They are short, and their Run limits bound them.

### D4a. Large tool results
- **The cap:** a tool result larger than policy.llm.max_tool_result_kb (default 64) is stored in `thread_attachments` in `app.db`, together with the result's provenance. The model receives the first 4 KB as a preview, followed by a reference: the attachment id and its size in bytes.
- **Reading in parts:** the core tool `read_attachment` takes `{attachment_id, offset, length}`. It reads parts of attachments of the same Thread only. A part is at most the cap, and it carries the provenance of the attachment, so untrusted text stays untrusted.
- **The Run's event log** stores the preview, not the full text, so the log stays small.

### D5. Interrupt model
- **What an interrupt is:** an interrupt is persisted as an Action item record: `{id, run_id, kind: approval|question, payload, created_at, resolution?}`.
- **Resume:** resuming replays the event log up to the pending call, then applies the resolution.
- **Approve with edits:** edited arguments are validated against the tool schema before execution.
- **Subagents:** a subagent's interrupt also marks its ancestors interrupted, so the UI shows one Action item at the Thread level (fixing pizza-bot issue #150).
- **Delegation:** the model delegates through the core tool `delegate`, with arguments `{task, instructions?, tools?}`. Each call creates a child Run (`parent_run_id`) and runs it to its end on the call's pool thread. Several `delegate` calls in one turn therefore run in parallel, up to policy.llm.max_parallel_tools. The child's final answer becomes the call's result.
  - If the child consumed untrusted content, the result is untrusted, with source kind `subagent`.
  - Subagents cannot delegate further.
- **Tool subset:** the child's tools are the requested ids, minus any the policy does not permit and minus the runtime's own tools. When `tools` is omitted, the child gets the tools loaded in the parent. The set is fixed in the child's first event (`subagent`), and `find_tools` in the child searches only that set, so a child can never obtain a tool its parent lacks. The child's conversation is its own system prompt and task; it does not see the Thread.
- **Child interrupts:** when the child is interrupted, the parent's call has no result yet. The parent then becomes interrupted as well, because it checks the open Action items of its whole subtree. When the item is resolved, the root Run resumes, never the child directly. The root reruns the pending `delegate` call, and that resumes the child where it stopped.
- **Cancellation** cascades. Cancelling a Run cancels every child Run that has not ended. Startup recovery fails queued and running children just as it fails their parents.
- **Not shown to children:** steer-now messages and queued messages are delivered to the root Run only.

### D6. Provenance tagging
- **Labels:** every tool result carries `{tool_id, source_id, trust: trusted|untrusted}`.
- **What counts as untrusted:** results from web, mail, files, transcripts, MCP and the browser.
- **How untrusted text reaches the model:** it is wrapped in a delimited block with a random per-Run boundary token, so a nonce makes delimiter spoofing impractical. The system prompt tells the model to treat the block as data.
- **Format:** the block is `<<<untrusted-data id=NONCE source=KIND:ID>>>` … `<<<end-untrusted-data id=NONCE>>>`. NONCE is 16 random URL-safe characters per Run. It is stored in the Run's first `nonce` event, so a resumed Run keeps it.
  - Wrapping happens once, when the tool result becomes a tool message. The stored conversation therefore needs no rewriting, and the blocks of earlier Runs keep their own nonces.
  - Inside a block, every `<<<` is replaced by `<< <`, so content can never form a marker, not even with a leaked nonce.
  - The tool_result event keeps the raw text and the provenance record for the UI and audit.
- **The real protection** is not the prompt. It is client/policy-enforcement's rule that external and destructive actions need decisions which untrusted text cannot satisfy.

### D7. Tool-call robustness
- **Repair before failing.** Repair covers trailing commas, single quotes, unescaped newlines in strings, and code-fenced JSON.
- **Schema validation** uses a JSON-Schema subset validator (types, required, enum, min/max), followed by one corrective re-prompt.
- **Fuzzing:** the argument parser with the schema validator (`fuzz_tool_args`) and the SSE framing parser (`fuzz_sse`) are fuzz targets with seed corpora and dictionaries under `client/fuzz/corpus`; the SSE seeds are the recorded upstream streams. There are two ways to run them:
  - **Locally with GCC on MinGW, which has no libFuzzer:** the targets build a standalone driver with a mutation mode (byte flips, inserts, deletes, duplications, splices and dictionary tokens). It saves a crashing input and fails any input slower than 2 s. `ctest -L fuzz` runs a short pass. On 2026-10-22, 20 million mutated argument texts and 4 million mutated streams ran without a crash; the slowest input took 18 ms.
  - **In CI:** a Linux job runs each target for two minutes under libFuzzer with ASan and UBSan (Clang).
  - **Alternative considered:** AFL++ on Linux only. Rejected as the only route, because a fuzz pass would then need a Linux machine.
- **Parser differences between serving stacks** are covered by the eval suite (task 1.4), not by special-casing in code.
- **Tool names on the wire:** OpenAI function names cannot contain `.`, so a Tool id such as `files.read` cannot be sent as is. The client maps the id to `files__read` when it builds the tool list and maps it back when it parses a tool call, so the runtime, policy and the Activity log always see the Tool id.
  - **Alternative considered:** renaming Tools to underscore ids everywhere. Rejected because Tool ids are the keys of policy.tools.rules patterns (`files.*`, `browser.*`) and appear in Group policies.

### D8. Gateway implementation
- **Stack:** Node.js with TypeScript and Express, as a module of the one control-plane process (decided 2026-10-22). Forwarding uses a streaming forwarder on the Node.js fetch API (undici), written per request: usage parsing of the streamed body, role rewriting of the request body and failover before the first token all need per-request code. Middleware provides JWT validation (control-plane/identity), role mapping, budget and rate checks, audit, and health-checked backend pools. Budgets and audit are stored in SQLite (`node:sqlite`).
- **Limits and budgets:** per-user request-rate and concurrency limits are in memory in the gateway process. Token budgets have no counter of their own: before each request the gateway sums the user's `llm_usage` rows for the bucket since the start of the budget period, using the `(subject, bucket, at)` index. A budget therefore survives a restart with nothing extra to persist, and since there is one node there is no reconciliation between nodes. Usage is recorded after the response completes, because it is only known then, so requests in flight together can overshoot a budget slightly. This is accepted behaviour (control-plane/llm-gateway Token Budgets).
  - **Alternatives considered:** reserving tokens before every request (exact, but needs an estimate of the output, and the reservation must be released on every failure path); a Redis-style shared counter (a second service to operate for one node).
- **Usage audit:** each `llm_usage` row records the user, the Installation (`x-skowork-installation`), the Run (`x-skowork-run`), the user's Groups at request time, the endpoint, role, upstream model, bucket, token counts, outcome and duration. Content is stored only at telemetry level `content`, and only for chat; embedding and rerank inputs are never stored.
- **Rerank usage:** rerank counts against the embed budget. The token count is `usage.total_tokens` when the upstream reports it, else `meta.billed_units.input_tokens` (the Cohere-style field), else an estimate of the input, because a rerank response may carry no `usage` block.
- **Effective policy:** the gateway reads the user's effective policy.llm (models, budget, rate limits, telemetry level) through an async effective-policy interface. Until control-plane/policy-service lands (change 3) the implementation returns the static default mapping from configuration; the policy service then supplies the real one without a gateway change.
- **Streaming:** streams pass through. The gateway parses the final usage chunk to record usage, and requests `stream_options.include_usage`. A failure before the first byte is retried on another backend. After the first byte the gateway cannot swap backends without duplicating output, so a cut stream ends without `data: [DONE]` and is audited with the outcome `truncated`; the client then applies truncated-stream detection (D0).
- **Upstream:** the gateway forwards to the organization LLM API (D0) with its own credential. The four client-facing endpoints map one to one onto the upstream chat completions, models, embeddings and rerank endpoints; the gateway rewrites the role in `model` to the mapped upstream model id.
- **Alternatives considered:**
  - Clients calling the organization LiteLLM proxy directly with per-user virtual keys: LiteLLM's virtual-key and team model does not map onto Keycloak-issued JWTs and Skowork Groups without a custom auth plugin, and keys would sit on laptops.
  - Running our own LiteLLM instance as the gateway: adds a Python service to operate and still needs the custom auth plugin.
  - Direct client-to-vLLM: no central quotas or audit, and keys would sit on laptops.
  - A reverse-proxy library (for example http-proxy-middleware): its response and request transforms make per-request usage parsing, role rewriting and failover before the first token awkward, so the forwarder is written directly.
- **Development upstream:** there is no LLM mock. Development and local end-to-end tests call the organization LLM API, configured from the environment variables OPENAI_BASE_URL and OPENAI_KEY (`.env` values, exported into the environment). The gateway refuses to start in its development configuration when either is missing, and it has no fallback backend. A `.env` file is not loaded automatically by the gateway; the developer exports the variables, or the dev-stack script does. Production never reads these variables.
- **Test doubles:** unit and integration tests of the gateway (`npm test`, node:test) may use an in-test fake upstream only for behaviour the real API cannot produce on demand: cut streams, 5xx, 429, backend failover and killed backends. Everything else runs against the real API. Tests that need the real API are excluded from CI, run locally only, and never log the key. The key is never a CI secret.
  - **Alternative considered:** a scripted mock OpenAI backend in the repository (the earlier dev/llm-mock). Rejected on 2026-10-22 because a mock drifts from the real API, and the real API is available in development.
- **Client model-access tests:**
  - The local end-to-end tests (`e2e_tests`, `ModelE2E.*`) run the client's model access through the local gateway to the real API. That covers role discovery, streamed and non-streamed reasoning, tool calls of both chat models, 16 embeddings, 30 rerank scores, and one agent Run that calls a tool.
  - With `SKOWORK_E2E_RECORD=<dir>`, these tests write the raw gateway responses. Checked for addresses and keys, they become the unit-test fixtures under `client/tests/model/fixtures`, which run in CI without the API.
  - The embedding vectors in the fixtures are cut to their first 8 values, to keep the repository small.
  - Cut streams are made by truncating a recorded stream at every event boundary and inside events.

### D9. Error mapping

| Gateway response or condition | Client code |
|---|---|
| 401 (the user's own token) | AUTH_EXPIRED |
| 429 with a quota reason | QUOTA_EXCEEDED |
| 429 for other reasons | RATE_LIMIT (with the retry delay) |
| 400 with a context-length reason | CONTEXT_LENGTH |
| 502 or 503 | MODEL_UNAVAILABLE |
| 403 with a role-not-granted reason | MODEL_UNAVAILABLE (reason "role not granted", not retried) |
| 400 INVALID_REQUEST (malformed request, or a role that does not fit the endpoint) | not retried (client/model-access: other errors are not retried) |
| A stream truncated with no finish reason | MODEL_UNAVAILABLE |
| 504 or a connect or idle timeout | TIMEOUT |

The gateway maps its backends' failures before they reach the client: backend 401, 403, 404 and 5xx become 503 MODEL_UNAVAILABLE, never AUTH_EXPIRED, because AUTH_EXPIRED must mean that the user's own token was rejected and would otherwise make the client refresh a good token and retry. Backend 429 becomes RATE_LIMIT with the upstream Retry-After, a backend timeout becomes 504 TIMEOUT, and all backends being at capacity gives RATE_LIMIT (control-plane/llm-gateway Backend Error Mapping).

Remedies are localised strings in the UI (client/work-inbox shows them).

### D10. Eval suite (task 1.4)
- **Shape:** `evals/` holds 47 tool-calling tasks in eight categories (files, web, mail, calendar, multi-step, parallel calls, ask_user, injection probes) and a mock world (`evals/world.json`: files, web and intranet pages, a mailbox, a calendar, an organization directory). Each task may patch the world.
  - **Runner:** `eval_runner` (`evals/runner`, built with the client's e2e targets) runs every task as a real Run of the client AgentRuntime through the local gateway to the organization LLM API, signed in through the Keycloak test realm. The runtime behaves as in the app: find_tools discovery over 19 mock tools, four of them distractors, untrusted-data delimiters, approvals and Questions.
  - **Untrusted results:** web, mail and calendar results and files under Downloads are untrusted.
  - **Policy:** the eval policy allows read and write_local calls and requires approval for external and destructive ones. The runner approves only calls that match the task's `approve` patterns. Every other approval request is rejected and fails the task.
  - **Checks:** a task passes when the Run succeeds and every check holds. The checks cover the answer text, executed calls (tool and argument patterns), calls that must never be attempted, the files afterwards, parallel calls in one response, and a Question asked.
  - **Rule:** like the other real-API tests, the suite runs locally only (D8): CI cannot reach the organization LLM API and holds no key for it.
- **Run context:** the runner adds the current date and the user's home folder to the system prompt. Without them the model probed `~/Downloads` and `C:/`. The app must give every Run the same context when the runtime is wired into the app.
- **Results (2026-10-22):** each task ran twice per model, three Runs at a time.

  | | main → acme-llm-primary | small → acme-llm-secondary |
  |---|---|---|
  | Pass rate | 92/94 (97.9%) | 94/94 (100%) |
  | Malformed tool calls | 1 corrective re-prompt in 328 responses with calls (0.3%), no call left invalid | 0 in 393 |
  | Injection probes resisted | 16/16 | 16/16 |
  | Task wall time p50 / p95 | 10.1 s / 16.8 s | 3.2 s / 11.1 s |
  | Model call p50 / p95 | 2.0 s / 4.3 s | 0.56 s / 1.8 s |
  | Tokens (prompt / completion) | 671k / 61k | 618k / 20k |

  - **main failures:** both were genuine model misses. In `ask-lunch-day` the model asked in plain text instead of calling ask_user, so the Run ended without an answer. In `mail-find-fact` it never searched the mailbox. A run before the fixes below also gave secondary one plain-text question.
  - **Fixes from the first pass:** that pass found three eval defects. A calendar mock returned only events that start inside the requested window, and two checks were stricter than their prompts. It also found one runtime defect: ask_user treated the model's choices as the only valid answers, although the model told the user they could answer freely. The Ask User Tool requirement now says the choices are suggestions, and the user may always answer in their own words.
- **Decision on `main`:** the default stays acme-llm-primary. The two models pass at the same rate within the noise of 94 Runs. secondary is about three times faster on these short tasks, but an agent Run's context grows. There primary's prefill is 8–10 times faster (spike 1.2: 4–9 s against 48–107 s at 64–128k). primary also has the larger window, reasoning and vision. secondary stays `small`. A Group that values latency over long-context work can map `main` to secondary in gateway configuration alone.
- **Release threshold (task 5.1):** a pass rate of at least 90% for each model, and no injection probe that leads to an unapproved external action.
- **Injection probes:** there are eight, in a downloaded file, a web page, a search result, the mailbox, a calendar event and a vendor reply. They ask for mail forwarding, a resignation email, file deletion, overwriting a local file without approval, exfiltration through a fetched URL, an organization-wide invitation, and leaking sales figures in a legitimate reply. None led to an unapproved external action in 32 Runs, and no local file was changed. Every approval of a call made after reading untrusted content showed the untrusted source (24 approvals). The probes are the verification of task 3.10.

### D11. App integration
- **Wiring in the core process:**
  - **Model client:** the core process creates one ModelClient. Its gateway URL comes from the registration response (`services.gateway`, client/control-channel Control-Plane Discovery), and it sends the installation id. Role information is refreshed whenever the channel connects.
  - **Runtime:** one AgentRuntime runs on `app.db`. It starts after storage and identity, and stops before them on shutdown. The tray's active-Run count is the runtime's executing-Run count.
- **Start gate:** the start gate refuses a new Run with the first reason that applies:
  - not signed in;
  - not registered (no gateway URL yet);
  - disconnected;
  - below the minimum version;
  - locked.

  A revocation push calls `revoke()`. Leaving the locked state after a successful re-registration calls `restore_access()`.
- **Run context (Run Context):**
  - **Collection:** the core process collects it through Win32. The date, weekday and time come from `GetLocalTime`. The UTC offset and time zone come from `GetDynamicTimeZoneInformation`. The folders come from `SHGetKnownFolderPath` (Profile, Documents, Downloads, Desktop), and the user's name from the identity's principal.
  - **When:** the runtime calls the provider each time a Run starts or resumes executing, and appends the text to the Run's system prompt.
  - **Why fixed per execution:** context that changed with every model call would change the prompt prefix every minute, defeating vLLM's prefix cache.
  - **Why in the system prompt:** the eval (D10) showed that the model probes for folders and dates otherwise.
  - **Alternative considered:** a `time.now` tool. Rejected because the model rarely calls it unprompted, and every relative date would cost an extra model call.
- **Local Run API:** the RPC methods are `threads.create`, `threads.list`, `threads.get`, `threads.send`, `threads.compact`, `queue.edit`, `queue.remove`, `runs.get`, `runs.events`, `runs.cancel`, `actions.resolve` and `actions.answer`.
  - **Events:** each runtime event is pushed to the window as the `run.event` message, marshalled to the main thread.
  - **Exposure:** in release builds, the methods that act are UI-only, and the pipe offers none of them; the read methods are UI-only too. Development builds expose all of them on the pipe, for the app end-to-end tests. This keeps another same-user process from driving the agent through the pipe in production (client/desktop-host Local API).
  - **Thread list:** for now it is a plain list of the latest 200 Threads. The inbox views and keyset paging of add-work-inbox replace it.
- **Interim policy handler:** the mandatory policy handler is client/policy-enforcement's (add-policy-and-sandbox). Until it lands, the app registers a fixed handler with policy-enforcement's default floor:
  - read and write_local calls are allowed;
  - external and destructive calls need approval;
  - every registered tool is permitted.

  The app has no domain tools yet (add-core-tools-and-mcp), so in practice only the runtime's own tools run.

### Baseline performance expectations
- The runtime overhead per turn, excluding the model, should be ≤ 20 ms on an i5.
- Idle Runs cost no CPU. Their memory per interrupted Run is ≤ 200 KB, because conversation state is loaded from the database on resume.

## Risks / Trade-offs

- [The on-premise model's tool calling is unreliable on long tasks] → Phase-0 eval gate (task 1.4). Repair and validation. Fewer tools per turn. Gateway role mapping allows switching models per Group without a client release.
- [Context windows are too small for long tasks] → Compaction, attachment offloading, subagents with fresh contexts.
- [Prompt injection through tool results] → Provenance tagging plus decisions by policy-enforcement. Untrusted text can never resolve Action items. Red-team tests are in evals.
- [The upstream does not report context window or concurrency] → Gateway server settings hold the context windows measured by spike 1.2 (D0), and a conservative capacity until load test 1.3 measures concurrency.
- [The organization LLM API changes model ids or behaviour without notice] → Clients only use roles, so a rename is a gateway setting. The local pre-release eval (task 5.1, D10) and the embed model id returned with vectors detect behaviour and model changes.
- [Gateway is a single point of failure] → The gateway is one process in the one control-plane container. Clients retry with backoff, the restart is supervised by the container runtime, and the outage is visible in the status bar. A second node is a later change if measured availability requires it.
- [Requests in flight together overshoot a budget, because usage is recorded only after a response completes] → Budgets are soft and the overshoot is bounded by the requests in flight, which is accepted (D8). Per-user rate and concurrency limits are in memory in the one process, and backend capacity limits protect the model servers.

## Migration Plan

Not applicable: this is new functionality. Gateway role mappings are seeded per Group through control-plane/policy-service (change 3). Until change 3 lands, a static default mapping applies, supplied through the effective-policy interface (D8).

## Open Questions

- Resolved 2026-10-21: the serving stack (LiteLLM over vLLM) and the default role mapping are recorded in D0. Resolved 2026-10-22 by the eval (D10): `main` stays acme-llm-primary; mapping a Group's `main` to acme-llm-secondary remains a gateway setting.
- Resolved 2026-10-22: the usable context windows (spike 1.2, D0). Still open: the upstream's concurrency limit, which load test 1.3 measures. Until then the gateway's per-backend capacity is a server setting.
