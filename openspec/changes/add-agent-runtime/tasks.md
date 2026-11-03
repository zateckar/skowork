# Tasks

## 1. Validation spikes

- [x] 1.1 Probe the organization LLM API for basic capabilities: tool calls on acme-llm-primary and acme-llm-secondary, vision on acme-llm-primary, streamed reasoning, the `data: [DONE]` stream terminator, 4096-dimension vectors from acme-llm-embedding, and `relevance_score` results from acme-llm-reranker. Verified 2026-10-21 by probe; results recorded in design.md D0
- [x] 1.2 Measure the usable context length of acme-llm-primary and acme-llm-secondary (needle-in-haystack at 8k, 32k and 64k), because the upstream does not report it. Verify the results are recorded in design.md D0, the gateway server settings are updated, and the compaction threshold is confirmed
- [ ] 1.3 Load-test the organization LLM API through a prototype gateway with 50, 200 and 500 concurrent streaming sessions, and measure embedding throughput in chunks per second. Verify p95 time-to-first-token, tokens/s and chunks/s are recorded, and size the initial gateway rate limits and per-backend capacity from them
- [x] 1.4 Build a 30–50 task tool-calling eval suite under `evals/` (file, web, mail-mock, multi-step, parallel calls, ask_user, injection probes). Run it against acme-llm-primary and acme-llm-secondary, and record the pass rate, malformed-call rate and latency in design.md. Verify the default `main` mapping is confirmed or changed from the results

## 2. Model access (client/model-access)

- [x] 2.1 Implement the WinHTTP streaming SSE client with connect and idle timeouts and truncated-stream detection. Verify with unit tests on recorded streams, including a cut stream
- [x] 2.2 Implement the error taxonomy mapping and retries with backoff, Retry-After and the auth refresh. Verify with a fake gateway that returns each error class
- [x] 2.3 Implement tool-call JSON repair and schema validation with a single corrective re-prompt. Verify with unit tests and a fuzz target over the argument parser
- [x] 2.4 Implement role discovery from the model list, image downscaling and usage accounting including reasoning tokens. Verify that a 4K screenshot is downscaled, that an image sent with a non-vision role is refused, and that usage totals appear per Run
- [x] 2.5 Implement embedding and rerank requests by role. Verify against the real embedding and reranking models locally, and with unit tests on recorded responses, that 16 chunks return 16 vectors with model id and dimension, and that 30 passages return 30 scores in input order
- [x] 2.6 Implement reasoning output handling for streamed and non-streamed responses. Verify with recorded acme-llm-primary streams that reasoning is delivered separately and is absent from the next turn's request

## 3. Agent runtime (client/agent-runtime)

- [x] 3.1 Implement the Run state machine and the append-only event log in `app.db`. Verify that transition tests reject illegal transitions, that a crash during a Run marks it failed on restart, and that queued Runs can go to failed or cancelled on restart, cancellation or revocation
- [x] 3.2 Implement the tool-calling loop with parallel calls and per-Run limits. Verify with a scripted fake model
- [x] 3.3 Implement the hook registry with the mandatory, non-removable policy handler and the fail-closed rule. Verify that a test without a policy handler refuses to start, and that a throwing handler denies the call
- [x] 3.4 Implement the Risk class declaration, with undeclared tools defaulting to destructive. Verify with a unit test
- [x] 3.5 Implement `find_tools` lazy loading over permitted tools. Verify that denied tools are never returned and that the per-turn schema size is ≤ 4k tokens
- [x] 3.6 Implement compaction at 85%, the manual "Compact Thread" action, BeforeCompaction, and attachment offloading above policy.llm.max_tool_result_kb. Verify with a long scripted Run and a manual-compaction test
- [x] 3.7 Implement subagents (child Runs, tool subset, parallel batches). Verify that a subagent cannot obtain a tool its parent lacks
- [x] 3.8 Implement `ask_user`, client-raised Questions with fixed choices, the required `rationale` argument, approval interrupts with the four resolutions, and idempotent resume across restart. Verify that a call with no rationale is rejected, and that restarting while interrupted and then approving executes only the pending call
- [x] 3.9 Implement message queue and steer-now, and cancellation by Run id that terminates child processes. Verify that a stale cancel id does not affect a newer Run
- [x] 3.10 Implement provenance tagging with a per-Run nonce delimiter. Verify that injection probes in the eval suite do not cause unapproved external actions (verified 2026-10-22: 32 probe Runs, design D10)

## 4. LLM gateway (control-plane/llm-gateway)

- [x] 4.1 Implement the Node.js and TypeScript gateway (Express) with JWT audience validation, the four endpoints, role mapping with the default mapping, the filtered model list (capabilities from server settings where the upstream reports none) and size checks. Verify with `npm test` (node:test) against an in-test fake upstream for backend faults and against the real API locally for the happy paths, and verify that no response contains the upstream address or credential
- [x] 4.5 Configure the organization LLM API upstream from server settings (development: `.env` OPENAI_BASE_URL and OPENAI_KEY). Verify with a smoke test against the real API that is excluded from CI and never logs the key
- [x] 4.2 Implement the chat and embedding token budgets and rate limits with QUOTA_EXCEEDED and RATE_LIMIT responses. Verify the reset time in the response, that embedding usage leaves the chat budget unchanged, and that budget counters in SQLite survive a gateway restart
- [x] 4.3 Implement usage audit with content logging gated by telemetry level. Verify that no content is stored at level `metadata`, and that embedding and rerank inputs are not stored at level `content`
- [x] 4.4 Implement backend pools with health checks and failover before the first token. Verify that killing one backend causes no failed requests in a soak test

## 5. Integration

- [ ] 5.1 Run the eval suite end to end (client runtime to gateway to on-premise model) locally before each release with `eval_runner --threshold`, because CI cannot reach the organization LLM API and holds no key for it (design D8, D10). Verify that the pass rate meets the threshold set in task 1.4
- [x] 5.2 Run `openspec validate --all --strict` and fix any findings

## 6. App integration

- [x] 6.1 Wire the model client and the runtime into the core process with the start gate and revocation (design D11). Verify with an app end-to-end test that a message sent while signed in and connected produces an answer, and that a Run is refused with the reason while disconnected
- [x] 6.2 Implement the Run context (Run Context). Verify with unit tests that the system prompt carries the provided context and that it stays the same across the model calls of a Run, and in the app end-to-end test that the model answers a question about today's date correctly
- [x] 6.3 Implement the local Run API and event push (Local Run API). Verify the methods with the app end-to-end test over the development pipe, and verify with a unit test that a release build's pipe does not offer the methods that act

## Workflow follow-up

- Archive this change after review with `openspec archive add-agent-runtime --yes`, then check that the merged specs read coherently.
