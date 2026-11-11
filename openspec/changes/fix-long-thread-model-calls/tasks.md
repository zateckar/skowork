## 1. Validation spikes

- [x] 1.1 Check prefix caching on the organization LLM API: send the same 40k-token prompt twice, then the same prompt with one extra message, and read `usage.prompt_tokens_details.cached_tokens` and the time to the first token. Verify: record in design.md whether caching is on and whether LiteLLM reports cached tokens; if it is off, record the request to the upstream owner
- [x] 1.2 Check abort propagation: start a 100k-token streamed request through LiteLLM, disconnect after 5 s, and immediately send a short request. Verify: record in design.md whether the short request waits for the abandoned one
- [x] 1.3 Reproduce the failure: a growing Thread sent with the old and the new prompt layout (dev/scripts/llm-thread-spike.mjs). Verify: record the time to the first byte per Turn and the TIMEOUT failures before the change in design.md

## 2. Gateway

- [x] 2.1 Stream Keep-Alive (D1): headers after acquiring a backend, `: keep-alive` on the interval, errors after headers as an error event. Verify with tests against a local fake upstream: delayed first chunk, failure before the first byte with failover, all backends failing, a request refused before streaming
- [x] 2.2 Stream Time Limits (D2): first-byte, idle and maximum limits; `requestTimeoutMs` no longer aborts a stream body. Verify with tests: a 7-minute stream with short gaps completes (simulated clock), no first byte gives TIMEOUT, an idle gap after the first byte is cut and truncated
- [x] 2.3 Prompt Cache Audit (D7): `ttft_ms` and `cached_tokens` columns and migration. Verify with tests: values recorded from a fake upstream's usage; empty when not reported

## 3. Client

- [x] 3.1 Request Timeouts and Retries (D3): read policy.llm.timeouts.*, apply changes to the next request, retry TIMEOUT once. Verify with unit tests using the in-test transport fake: policy value applied, TIMEOUT attempts counted
- [x] 3.2 Stream Errors and keep-alives: comment-only streams and an error event map to typed errors. Verify with unit tests and extend the SSE fuzz corpus with comment and error frames
- [x] 3.3 Stream summary requests (D5). Verify with a unit test that the summarizer request has `stream: true` and that the summary is assembled from deltas
- [x] 3.4 Stable Prompt Prefix and Run Context (D4): stable system prompt, time line on the Run's starting message, summary nonce from the compaction record, raw tool-call arguments. Verify with a unit test that builds Turn 1 and Turn 2 requests across a change of minute and asserts the byte-prefix property, and one for a compacted Thread
- [x] 3.5 Lazy Tool Loading per Thread: load order kept, bound drops the earliest loaded, tools that policy denies are dropped. Verify with unit tests for each scenario
- [x] 3.6 Run Retry (D6): `threads.retry` resumes the failed latest Run; failed model output is excluded from later requests. Verify with unit tests: one user message after retry, completed tool calls not re-executed, retry refused for an older Run

## 4. Acceptance

- [ ] 4.1 Repeat spike 1.3 after the change on the local stack against the real organization LLM API. Verify: no TIMEOUT over 30 Turns, cached tokens growing per Turn in the gateway audit (if 1.1 found caching on), and the time to the first byte recorded per Turn in design.md
- [x] 4.2 Run the eval check for unprompted time mentions (design risk). Verify: 0 of 5 prompts mention the time unprompted
- [x] 4.3 Run `openspec validate --all --strict`
