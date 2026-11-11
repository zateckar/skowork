# Proposal

## Why

On 2026-11-10 the product owner asked for two gateway features:

- **Model fallback.** When the primary model keeps timing out, requests should fall back to a secondary model instead of failing. Today the gateway only fails over between connections that offer the same model id (control-plane/llm-gateway Backend Failover). When the only model behind a role is overloaded, every Run in the organization fails.
- **Reasoning effort.** It should be possible to set `reasoning_effort` for each configured model. Both organization chat models reason by default and spend many tokens on it. A spike on 2026-11-10 measured 1,348 reasoning tokens and 9.7 s for one short arithmetic question on the primary model, and 3,153 tokens and 20.8 s on the secondary. With `low` the primary answered the same question correctly in 2.0 s. No setting exists today, and the client never sends the parameter.

## What Changes

- **Reasoning effort.** Each chat model of an LLM connection gets an optional reasoning effort, set by a platform-admin in the admin console.
  - The gateway sends it upstream as `reasoning_effort` on every chat request routed to that model.
  - Saving a value checks it against the real model with a tiny request, because each model accepts different values.
- **Fallback model.** Each chat model gets an optional fallback model and a fallback wait (default 60 seconds).
  - When the model has not sent a first byte within the wait, or fails before its first byte with TIMEOUT, MODEL_UNAVAILABLE or a capacity limit, the gateway sends the request to the fallback model instead. This requires that the request fits the fallback model's context window, tools and images.
  - After such a fallback, requests for the model go straight to the fallback model for a cooldown (server setting, default 2 minutes).
  - A fallback has no fallback of its own.
- **Audit.** The gateway usage audit records which model answered and whether it was a fallback.

Out of scope:
- Showing the fallback in the client. The client is not told; its requests and budgets are unchanged.
- A per-request or per-user reasoning effort, and the client sending `reasoning_effort`.
- Fallback chains longer than one step, and fallback on errors after the first byte. A stream cut after its first byte is still reported as truncated.
- Changes to role mapping (policy.llm.models) or to failover between connections that offer the same model id.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `control-plane/llm-gateway`: Reasoning Effort Setting, Reasoning Effort Check, Reasoning Effort In Connection Test, Fallback Model Setting, Model Fallback, Fallback Request, Fallback Cooldown and Fallback Audit are added.
- `control-plane/admin-console`: Model Fallback And Effort Administration is added.

## Impact

- **Roadmap:** 38 [Hardening]. It depends on add-llm-connections (connections and model settings) and on fix-long-thread-model-calls (stream keep-alive and first-byte limits), and is archived after them.
- **Code:**
  - control-plane/src/gateway/connections.ts: model settings, validation and the probe request.
  - control-plane/src/gateway/gateway.ts: request rewriting, fallback, cooldown and audit.
  - control-plane/src/db.ts: migration.
  - control-plane/src/config.ts: the cooldown setting.
  - control-plane/admin: the model editor in the Connections view.
- **Data:**
  - Three nullable columns on `llm_models`: `reasoning_effort`, `fallback_model` and `fallback_after_s`.
  - Two columns on the gateway usage audit: `answered_by` and `fallback`.
- **Client:** none.
