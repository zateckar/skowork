# Design

## Context

The reviewed `agents.cpp` revision is a public-header package plus precompiled libraries (`BUILD:26-44`). `Context::registerTool`/`registerToolRegistry` builds the catalog before agent execution (`include/agents-cpp/context.h:166-182`), the autonomous example performs registration before constructing the agent and then reuses it (`examples/autonomous_agent_example.cpp:94-238`), and the combined example reuses one Context over several tasks (`examples/combined_workflows_example.cpp:101-221`). Catalog storage is an ordered map.

The reference does **not** expose request-cache controls, backend affinity, raw Tool-call argument text, cached-token metrics, or a first-byte/idle split. Its default timeout is 30 seconds and its message-count compaction is not a substitute for Skowork's budgeted checkpoints. Its implementation of request formatting and compaction is not present. Its evaluation license prohibits production use, redistribution and reverse engineering. Consequently only the fixed-registration architecture informs this design.

Pi at commit `a635f390168324314e153c8c09fc5dfcccd35cc2` confirms and qualifies that architecture. Its small agent loop keeps finalized canonical messages separate from the provider-facing request projection (`packages/agent/src/agent-loop.ts:398-406`), resolves Tool-state changes deterministically (`agent-loop.ts:333-381`), and supports sequential or ordered parallel Tool execution. Its generic OpenAI-compatible projection still rebuilds the current leading Tool definitions, so a catalog change remains a prefix boundary. Pi's `sessionId` is only useful on backends that explicitly support it; generic endpoints do not automatically receive affinity headers or `prompt_cache_key`. Durable sessions and compaction are external to this low-level package. Independent Skowork designs remain in place for event persistence, policy, recovery, streaming, raw arguments and compaction.

Pi also rejects every Tool call when an assistant response hits its output-token limit (`packages/agent/src/agent-loop.ts:455-480`) because the arguments may be truncated. Skowork should review its `finish_reason` handling in a separate safety change; coupling that behavior change to the catalog fix would obscure cache acceptance.

Skowork already streams requests and summaries, sends keep-alives, separates time limits, retries a failed Run without duplicating its starting message, stores raw valid Tool-call arguments, and retains compaction nonces. It also records upstream time to first byte and cached prompt tokens in the gateway audit. The app-specific remaining defect is catalog churn:

1. The initial request has the reserved core Tools only.
2. A `find_tools` result adds app definitions before the stored conversation of the next request.
3. A later discovery adds file definitions before the whole expanded conversation again.
4. Source-contained assistant arguments make every cold prefill larger.

The existing Stable Prompt Prefix test avoids this transition and compares only Tool ids. The existing real-upstream thread spike sends no tools at all.

## Decisions

### D1. Make file and Local app Tools baseline definitions

The baseline catalog is:

1. The existing reserved runtime core Tools in deterministic registry order.
2. `files.read`, `files.write` and `files.edit` for reading and repairing app sources.
3. `apps.create` and `apps.open`, but only while `client/local-apps` offers them (runtime package available and policy.apps.mode on).

Only these five domain definitions set `core = true` at construction. `files.list/glob/grep` and `apps.list/logs/stop/delete` remain lazy; removing those Tools is unnecessary and would reduce product functionality. apps.create is essential because it allocates the app folder and grants only the current Run write access to it. apps.open is essential because it owns contained process startup, the owner-only browser launch and the MCP-confirmation/resume flow. These are host boundaries that generic files or shell calls must not bypass.

Existing `turn_tools()` availability and permission filtering remains authoritative, including for Child Runs. A legacy `tools_loaded` entry for a baseline Tool does not duplicate it because the loaded-tool loop already skips definitions marked core.

`core` must not accidentally change default delegation. Today `run_delegate()` initializes an omitted `tools` list from `loaded_` only, and baseline definitions never enter that set through `find_tools`. Change that default initializer to the union of the parent's available, permitted non-reserved core definitions and its Thread-loaded definitions; the existing grant pass removes reserved runtime Tools and unavailable or denied definitions. Duplicate legacy load ids have no effect because the result is a set. An explicit `tools` list does not receive this union: it remains the requested subset, filtered by the same grant pass. This preserves the effective Child Run set that an app/file-capable parent had before promotion, including for new Threads with no legacy `tools_loaded` events.

This mirrors the useful reference patterns without text-based workflow detection or an oversized universal catalog: every ordinary Thread or app-building Run sees the same small baseline from its first model call. Other built-ins and Connector Tools remain lazy. Their discovery continues to work and `find_tools` naturally stops returning baseline Tools because core Tools are not discovery candidates. Only available, permitted non-reserved Tools are loadable matches; the separate withheld explanation required by client/policy-enforcement remains possible without loading or granting the denied Tool.

**Alternatives considered:**
- *Send every Tool on every call:* rejected. Connector schemas can exceed 20k tokens and reduce small-model quality, the reason Lazy Tool Loading exists.
- *Pin only app Tools:* rejected. File discovery would still invalidate the enlarged app conversation. Both sides of app construction need stable definitions.
- *Pin every file/app Tool:* rejected. Listing, searching, stopping, deleting and log inspection are useful but not required on every model call. Pi's small-basic-tool pattern argues against that fixed-token and attention cost; retain those capabilities through lazy discovery.
- *Leave default Child Run inheritance tied to `loaded_`:* rejected. After promotion, baseline definitions would no longer be discovered into that set, so an omitted `tools` list would silently give Child Runs fewer capabilities than before; legacy and new Threads would also diverge.
- *Pin a Tool bundle only after recognizing an app request:* rejected. Text classifiers are brittle, a second model call would add latency, and Apps-view entry points do not cover app requests in an ordinary Thread.
- *Describe new Tools only in the newest message:* rejected. It would not provide a stable OpenAI function-calling contract and would make provider behavior less predictable.
- *Increase the schema budget or shorten schemas later without tests:* rejected. The combined baseline must fit the current 4,000-estimated-token budget by construction.

### D2. Treat catalog revisions and one-shot images as explicit prefix boundaries

Tool definitions precede messages, so adding a lazily discovered Tool cannot literally extend the preceding provider-visible prompt. Stable Prompt Prefix shall say that an **added** as well as dropped/withdrawn Tool starts a new catalog revision. Within a revision, complete Tool specs and previous canonical messages remain unchanged; appended Tool definitions retain their existing deterministic order.

Image Tool Results already requires a different, transient provider projection: the request immediately after an image-bearing Tool result appends an image message, and the next request omits it. Scope append-only comparisons to the provider-effective system message, Tool catalog and canonical stored messages, except for that one transient image projection, which is present only in that request. Its removal MUST leave every earlier canonical component byte-stable and requires no new storage or history behavior.

This is not a claim that all possible agent workflows have one constant prefix. The fix removes revisions from normal app/file construction and makes the remaining catalog and one-shot-image boundaries explicit and testable.

**Alternatives considered:**
- *Keep the current unconditional requirement:* rejected. It is unsatisfiable while adding a Tool before previous messages and ignores the accepted one-shot image projection.
- *Drop lazy discovery entirely:* rejected for the catalog-size reason in D1.
- *Persist or resend image history to satisfy an unconditional byte-prefix rule:* rejected. Image Tool Results owns the one-shot behavior and forbids storing image bytes; this change only accounts for that projection in prefix comparisons.

### D3. Test actual request content and real upstream reuse

Unit tests shall compare complete `model::ToolSpec` sequences (ids, descriptions and serialized schemas) and complete earlier messages, including raw arguments. They shall prove:

- apps.create and files.write can be called on the first two model turns, with no `find_tools` or `tools_loaded` detour;
- a source-writing sequence and following Run extend earlier messages with an unchanged complete catalog;
- a baseline Tool loaded by an old Thread appears exactly once;
- denied non-baseline Tools never become loadable matches or enter `tools_loaded`, while required withheld explanations still work;
- a delegate call with no `tools` list gives its Child Run the available, permitted baseline and Thread-loaded Tools, while an explicit list remains restrictive;
- missing runtime/package availability and policy withdrawal remove definitions without granting new capabilities;
- the existing one-shot image message is the only transient difference and its removal leaves complete earlier canonical request content unchanged.

The baseline budget test shall measure the actual `model::ToolSpec` values produced through `wire_spec()`, including the reserved core and a conservative variant in which every baseline definition carries the policy-dependent `rationale` addition. Raw `ToolDef` parameters are not the provider-effective input and are not sufficient for this measurement.

Before production flags change, a six-Turn `ModelE2E.AppThenFileCatalogRevisionsColdPrefillSourceHistory` control shall manually reproduce the current request sequence: warm a core-only catalog, add apps.create/open after their discovery messages, warm that catalog with app history, then add files.read/write/edit after further source-bearing history and recover with the full catalog. Use definitions selected from the actual `file_tools()` and `app_tools()` constructors and the same source fixture as the stable regression. Record cached-token reductions at both additions and correlate any TTFT increase. This is a before/after measurement control, not a claim to execute production `AgentRuntime` assembly.

The stable real `ModelE2E` regression shall construct synthetic requests with the actual five baseline domain definitions selected from the same constructors, plus deterministic source-bearing history. It is provider/cache acceptance, not a natural-language app-building test or an end-to-end test of production `AgentRuntime` request assembly. The unit integration tests above own the production catalog/history assertions; the synthetic test must not claim to replace them.

For every transport attempt, its recording transport shall retain the request body and either the response or transport failure. All three real cache tests shall use a `ModelClient` with `max_retries = 0` and `max_timeout_retries = 0`; any TIMEOUT is then visible and fails acceptance instead of being hidden by a successful retry. The transport shall parse final stream usage for `prompt_tokens` and `prompt_tokens_details.cached_tokens`.

Latency shall ignore gateway keep-alive comments. Record elapsed time to the first non-keep-alive SSE `data` event as a client-visible supplementary metric, and correlate each request's unique Run id with the gateway audit's `ttft_ms` for upstream first-byte latency. A raw HTTP-body callback is not an upstream first-byte measurement because keep-alives reach it during prefill.

The stable-catalog regression shall run 30 sequential Turns against the same gateway-answered model. Turns 2-30 each append one assistant files.write/edit call containing exactly 12 KiB of deterministic, indexed, non-repetitive JavaScript/HTML/CSS source and its short Tool result. This totals 348 KiB of added raw source before JSON escaping. The final response SHALL report at least 90,000 `usage.prompt_tokens`; an undersized fixture fails acceptance rather than silently weakening the regression. Use `max_tokens = 8`, record per-Turn cache counts and both latency measurements, and fail on any attempted TIMEOUT, an answered-model switch, or absent cache reuse. The first Turn is the cold baseline; later Turns must report cache hits. Counts may plateau while an appended Turn does not complete another KV block, but must be nondecreasing and end above their initial warm value. A separate five-Turn lazy-addition control supplies an unrelated definition and demonstrates that the specified catalog-revision boundary is real before recovering with the revised catalog.

**Alternatives considered:**
- *Use an unspecified growing source fixture:* rejected. Thirty small Turns could stay below the historical failure size and pass without exercising the relevant cold prefill; the source schedule and final provider token count are mandatory acceptance conditions.
- *Keep only a stable-catalog E2E:* rejected. Manually fixing the proposed baseline cannot measure the current core-to-apps-to-files discontinuity; the explicit pre-change app/file controls provide that baseline.
- *Compare Tool ids only:* rejected; policy-dependent schemas can change under the same id.
- *Measure raw Tool definitions for the schema budget:* rejected; runtime adds rationale to provider-effective schemas for calls that can require approval.
- *Compare serialized whole request bodies as string prefixes:* rejected; JSON array delimiters and following fields make this the wrong representation. Test provider-effective system/tools/messages projections, then verify reuse upstream.
- *Keep the normal retry policy in cache acceptance:* rejected; a successful retry would conceal the cold-prefill TIMEOUT this regression is intended to detect.
- *Treat the first raw HTTP chunk as upstream latency:* rejected; gateway SSE keep-alive comments deliberately arrive before upstream data.
- *Rely only on an app-building natural-language E2E:* rejected because model choices vary and may skip discovery or produce too few Turns. The deterministic source-history regression measures caching directly; existing app tests cover execution behavior.

### D4. Keep session persistence and speculative affinity outside this change

Skowork's canonical Thread event store and explicit compaction checkpoints are the durable state, just as Pi delegates durable sessions to its host. Model requests project that state to the provider; changing usage metadata or streaming progress must not rewrite history. A Thread id is useful for correlating cache evidence, but no `sessionId`, affinity header or `prompt_cache_key` is added without a spike demonstrating that the organization endpoint honors it.

**Alternatives considered:**
- *Forward Thread id as a universal vLLM session key:* rejected as unproven; Pi enables affinity fields only for supported backends, and the earlier secondary measurements already indicate non-sticky routing.
- *Move durable sessions/compaction into a new low-level agent abstraction:* rejected because it replaces working recovery and checkpoint behavior without addressing the catalog defect.

### D5. Archive only after referenced capabilities and owning requirement changes

Lazy Tool Loading is introduced by add-agent-runtime and revised by fix-long-thread-model-calls; Stable Prompt Prefix is introduced by the latter. Their deltas are the merge base for this change's MODIFIED requirements. Before archival, both changes must be archived and the accepted `client/agent-runtime` spec must contain both target headers. A syntactically valid standalone delta is not enough: archive refusal because a MODIFIED header is absent is a blocking dependency failure.

The baseline also references App Runtime and App Tools in `client/local-apps`. Archive the completed add-local-apps change first and verify `openspec/specs/client/local-apps/spec.md` is accepted. Archiving the agent-runtime dependencies alone would leave this change's capability reference unresolved in the merged specification.

**Alternatives considered:**
- *Relabel these requirements as ADDED in this change:* rejected. They already have owning changes, and parallel ADDED requirements would duplicate or conflict with those changes rather than revise their final text.
- *Archive this change before its dependencies:* rejected. OpenSpec cannot apply MODIFIED requirements whose target is absent, and the merged baseline must not reference an unaccepted `client/local-apps` capability.

## Performance and compatibility

The existing `estimate_tool_tokens()` calculation and `RuntimeConfig::tool_schema_token_budget = 4000` enforce the budget at lazy-load time. `LazyToolLoading.BaselineCatalogFitsSchemaBudget` measures **1,296 estimated tokens** with the actual policy schemas and **1,597 estimated tokens** with the rationale addition forced on every baseline definition, both below the 4,000-token budget. Measurements use complete actual schemas, not raw Tool definitions; exceeding 4,000 fails the budget test and blocks implementation rather than silently raising the limit.

The added baseline costs bounded input on every request, but app construction saves at least the app/file discovery model calls. On the primary's measured prefix-cache behavior, subsequent source-writing Turns should prefill only appended history rather than the whole Thread. Runtime assembly remains linear string/vector work and is far below model latency on the i5/16 GB baseline.

No persisted schema or policy format changes are needed. Older `tools_loaded` events and resumed Runs remain valid. Default Child Run delegation computes the same effective baseline set for old and new Threads without rewriting legacy events.

## Acceptance status

Local runtime/app verification passed all 20 focused tests: first-call app construction without discovery, complete schema and canonical-message prefix stability, one-shot image removal, permitted-only discovery and withheld explanations, baseline schema budgets, duplicate legacy loads, and default/explicit Child Run Tool sets for new and old Threads.

The real-organization-LLM controls and the 30-Turn regression remain unmeasured: interactive sign-in was completed, but the organization endpoint rejected the configured development credential with 403, leaving no healthy backend. At the user's direction, production implementation and local unit verification continue while those external acceptance tasks remain unchecked. No cached-token, TTFT, TIMEOUT or 90,000-prompt-token result is claimed here; they must be recorded after the real suite runs.

## Risks

- **[Risk] More baseline definitions reduce available context or model selection quality.** -> **Mitigation:** enforce the 4,000-token budget in unit tests; measure complete requests in the real regression; retain all unrelated Tools as lazy.
- **[Risk] An automatic retry hides a TIMEOUT from the cache regression.** -> **Mitigation:** disable both retry limits for all three cache tests, retain every transport attempt's outcome, and fail on any attempted TIMEOUT.
- **[Risk] Gateway keep-alives make a client-side first-chunk metric look warm during a cold prefill.** -> **Mitigation:** ignore SSE comments, label the first non-keep-alive event separately, and use the gateway audit's `ttft_ms` for upstream first-byte evidence.
- **[Risk] Non-sticky secondary replicas or fallback obscure acceptance.** -> **Mitigation:** require the same answered model throughout the real regression and report its per-Turn measurements rather than claiming that client code can repair upstream routing; an unchanged model id still does not prove replica affinity, so cache misses caused by routing are infrastructure failures rather than accepted results.
- **[Risk] Promoting Tools to core silently removes them from default Child Run delegation.** -> **Mitigation:** union available, permitted non-reserved core and Thread-loaded definitions only when the delegate `tools` argument is omitted; test both default inheritance and explicit subsets.
- **[Risk] A dynamic policy or package change still invalidates a Thread's catalog.** -> **Mitigation:** keep availability filtering deterministic, classify such changes as explicit catalog revisions, and cover removal/resumption tests.
- **[Risk] Reference code is copied despite its evaluation license.** -> **Mitigation:** implement only against Skowork's independently specified D1-D3; do not vendor, link or translate reference implementation code.
