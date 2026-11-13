# Proposal

## Why

Skowork needs an agent: a loop that plans with the on-premise LLM, calls tools, pauses for the user when needed, and keeps running in the background. Today nothing talks to the model. This change adds that runtime, plus the client and gateway path to the LLM, and keeps model keys and quotas central.

## What Changes

- Add the client agent runtime:
  - a tool-calling loop over OpenAI Chat Completions;
  - lifecycle hooks;
  - lazy tool loading with a `find_tools` meta-tool;
  - context compaction;
  - subagents;
  - the `ask_user` tool;
  - interrupts for approvals and questions;
  - steering, queueing and cancellation;
  - per-Run limits, Run states, background execution, and provenance tagging of untrusted tool results.
- Add client model access:
  - streaming calls to the control-plane LLM gateway, authenticated with the user's access token;
  - model roles `main`, `vision`, `small`, `embed` and `rerank`, with embeddings and reranking requests;
  - separate handling of reasoning output;
  - truncated-stream detection;
  - tool-call repair and validation;
  - typed error codes (client/work-inbox presents the remedies);
  - retries;
  - image input;
  - token usage accounting.
- Add the control-plane LLM gateway:
  - an OpenAI-compatible surface (chat completions, model list, embeddings, rerank) in front of the organization LLM API;
  - token authentication;
  - per-Group role-to-model mapping;
  - budgets and rate limits;
  - audit;
  - backend failover;
  - no model credentials on clients.

Out of scope:
- The inbox presentation of Threads, approvals and questions (client/work-inbox, change 4).
- How decisions are made (client/policy-enforcement, change 3). This change only provides the mandatory hook point.
- Concrete tools (changes 5 onwards).
- What is embedded, when, and how vectors are stored and searched (client/knowledge-graph, change 12). This change only provides the `embed` and `rerank` request paths.

## Capabilities

### New Capabilities
- `client/agent-runtime`: Run lifecycle, tool-calling loop, hooks, tool loading, compaction, subagents, interrupts, ask_user, steering, cancellation, limits, provenance
- `client/model-access`: the client's calls to the LLM gateway: roles, streaming, robustness, errors, retries, images, usage accounting
- `control-plane/llm-gateway`: the central OpenAI-compatible proxy in front of on-premise models: auth, model mapping, quotas, audit, failover

### Modified Capabilities
- None.

## Impact

- **Roadmap:** change 2 of 13 (MVP). It depends on change 1, `add-client-foundation`:
  - client/identity, for the user's access token that the gateway accepts;
  - client/local-storage, for persisting Threads and Runs;
  - client/desktop-host, for background execution;
  - client/control-channel, for the connected state that model access and Run start check.
- **Later changes:** change 3 plugs into the `BeforeToolCall` hook, change 4 presents Runs and interrupts, and changes 5 onwards register tools.
- **Policy keys owned here, delivered by change 3:** this change owns the `policy.llm.*` keys (`models`, `budget`, `rate_limits`, `run_limits`, `max_concurrent_runs`, `max_parallel_tools`, `max_tool_result_kb`, `max_images_per_request`, `timeouts`). They are delivered in the Policy bundle of change 3, add-policy-and-sandbox. Until that change lands, the client and gateway use the defaults stated in these specs and a static role mapping.
- **Forward references:**
  - client/policy-enforcement (change 3) defines decisions and the list of untrusted sources;
  - client/work-inbox (change 4) presents error remedies, Approvals and Questions;
  - the gateway's content logging depends only on `policy.telemetry.level`, owned by client/telemetry (change 7);
  - client/knowledge-graph (change 12) uses the `embed` and `rerank` roles;
  - client/browser-use (change 6) and client/computer-use (change 8) supply the image inputs that use the `vision` role;
  - client/scheduler (change 10) starts unattended Runs with origin trigger or remote.
- **New code:**
  - `client/runtime`, `client/model`
  - `control-plane/llm-gateway` module (Node.js with TypeScript and Express), part of the control-plane process
  - in-test fake upstreams for the faults the real API cannot produce on demand (cut streams, 5xx, 429, failover)
  - `evals/`: a tool-calling regression suite
- **Infrastructure:** network reachability from the gateway to the organization LLM API, and its credential stored only in the gateway's secret store (in development, the `.env` keys OPENAI_BASE_URL and OPENAI_KEY, exported into the environment). Development and local end-to-end tests use the real API. There is no LLM mock. Tests that need the real API run locally only, and the key is never a CI secret.
- **Security review:** the provenance tagging model, and the gateway's audit and content-logging switch.
