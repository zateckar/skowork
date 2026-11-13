# Proposal

## Why

Local app creation still reaches model TIMEOUT after several model calls although ordinary text-only Threads work. The remaining request-prefix defect is in Lazy Tool Loading: tool definitions precede conversation messages, but `find_tools` adds definitions between model calls. Every added definition therefore places different content before the entire earlier conversation and prevents vLLM from extending its previous prefix cache.

Local app creation exercises this repeatedly (`apps.*`, then `files.*`, then optional diagnostic tools), and its `files.write`/`files.edit` calls carry growing source code in stored assistant arguments. Each discovery makes the next cold prefill more expensive.

Two public agent harnesses provide useful contrasts. `agents.cpp` registers its tool catalog in a `Context` before starting the agent, then reuses that Context across tasks. Pi keeps a small basic agent core and projects finalized canonical messages into provider requests at the LLM boundary. Neither supplies universal vLLM prefix caching: `agents.cpp` exposes no raw-argument or cache contract, and Pi's generic OpenAI-compatible transport does not automatically forward `sessionId` as a worker-affinity key.

## What Changes

- Define a minimal stable baseline Tool catalog: the existing reserved core Tools, `files.read`, `files.write`, `files.edit`, and, while client/local-apps offers them, `apps.create` and `apps.open`. These five domain Tools cover app construction and repair without discovery calls. They are not interchangeable conveniences: apps.create grants the Run its folder; apps.open owns safe process/browser launch and MCP confirmation.
- Keep `files.list/glob/grep`, `apps.list/logs/stop/delete`, and all non-reserved Tools outside the baseline available through `find_tools` and per-Thread lazy loading. Preserve first-load order, permitted-only discovery and the 4,000-estimated-token Tool-schema budget. A Child Run with an explicit fixed Tool set remains restricted; when a delegate call omits that set, it inherits the parent's available, permitted non-reserved baseline Tools as well as its Thread-loaded Tools, so promoting a Tool to the baseline cannot silently remove it from the Child Run.
- Correct Stable Prompt Prefix so it also recognizes a newly added lazily discovered Tool as an explicit Tool-catalog revision and cache boundary. Between such revisions, full Tool schemas and earlier canonical messages remain stable, not only their ids. Account for the existing one-shot image projection as a narrow exception without changing Image Tool Results or image history.
- Add unit coverage for first-call app construction, legacy loaded-tool compatibility, permitted-only discovery, default and explicit Child Run Tool sets, policy/runtime availability, full request-prefix checks across several source-writing calls, and the one-shot image boundary.
- Before changing production registration, add a synthetic real-organization-LLM control that reproduces the current core-to-apps-to-files catalog transitions and records their cache and latency discontinuities. Add a stable-catalog regression using the actual five baseline file/Local app Tool schemas, appending one deterministic 12 KiB source chunk on each of Turns 2-30 and requiring at least 90,000 actual prompt tokens in the final request. Record cached prompt tokens, first non-keep-alive SSE latency and gateway `ttft_ms`; disable retries and inspect every transport attempt so no TIMEOUT or undersized workload can be hidden. These are provider/cache acceptance tests, not natural-language app-building or production request-assembly tests; runtime integration tests cover the latter.

Out of scope:
- Adopting, linking, copying or vendoring `agents.cpp`. Its evaluation license does not permit production use or redistribution, and its public API does not provide the required raw-argument or cache guarantees.
- Sending every Connector and built-in Tool on every request, workflow detection from user text, changing `find_tools` BM25 ranking, or increasing the Tool-schema budget.
- Upstream replica affinity, `prompt_cache_key`, model fallback changes, timeout changes, or a server-side Thread API.
- Tool-call repair history, invalid raw-argument preservation, time-zone history, compaction policy, Tool-result pruning, or offloading assistant arguments. Image-history redesign and changes to image storage, retention or vision behavior are also excluded; Stable Prompt Prefix only accounts for the existing one-shot image projection.
- Refusing Tool calls after `finish_reason = "length"`; that separate safety follow-up covers possible truncated arguments rather than prefix caching.
- Removing existing management Tools, redesigning Local app folder grants, or changing Local app execution, UI, policy decisions, approvals, or sandboxing.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/agent-runtime`: Lazy Tool Loading gains a stable baseline catalog without changing permitted-only discovery or default Child Run inheritance; Stable Prompt Prefix distinguishes stable catalog periods, explicit Tool-catalog revisions and the existing one-shot image projection.

## Impact

- **Roadmap:** 47 [Hardening]. Depends on add-agent-runtime, add-local-apps, add-core-tools-and-mcp and fix-long-thread-model-calls. This change follows their streaming, timeout and cache-audit work and narrows the prefix rule where dynamic discovery makes an unconditional guarantee impossible. Archive is blocked until add-local-apps has supplied the accepted `client/local-apps` spec, and add-agent-runtime and fix-long-thread-model-calls have been archived so the accepted `client/agent-runtime` spec contains Lazy Tool Loading and Stable Prompt Prefix; this change uses MODIFIED deltas for those requirements.
- **Code:** file and Local app Tool registration; default Child Run Tool inheritance; runtime comments and tests; model end-to-end cache regression.
- **Data/protocol:** no storage migration and no wire-protocol change. Existing Threads with `files.*` or `apps.*` in `tools_loaded` remain valid; a definition now in the baseline is emitted only once.
- **Performance:** the baseline schemas may use at most the existing 4,000-estimated-token combined Tool-schema budget. Measure the complete provider-effective schemas, including policy-dependent rationale additions, rather than raw Tool definitions. The additional fixed input is traded for eliminating app/file discovery calls and repeated cold prefills.
- **References reviewed:** `RunEdgeAI/agents.cpp` at commit `8ad0a20eda329301d74008948745695de3bffe13` and `earendil-works/pi` at commit `a635f390168324314e153c8c09fc5dfcccd35cc2` (including its directly imported provider projection), for architectural inspiration only; no code reuse.
