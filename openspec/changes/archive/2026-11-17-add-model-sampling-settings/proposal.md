# Proposal

## Why

Model vendors publish recommended sampling parameters for their models, and some models degrade without them (endless repetition, weaker agentic output). The recommendations for the model behind `skoda-llm-secondary` (thinking mode) are `temperature=1.0`, `top_p=0.95`, `top_k=20`, `min_p=0.0`, `presence_penalty=0.0`, `repetition_penalty=1.0`. The gateway forwards the client's body and can only override `reasoning_effort` per model, and the client's agent requests carry no sampling parameters, so these recommendations cannot be applied today.

## What Changes

- A platform-admin can store sampling defaults for each chat model: `temperature`, `top_p`, `top_k`, `min_p`, `presence_penalty` and `repetition_penalty`, each optional and range-checked.
- The gateway adds the stored values to every chat request routed to that model, for the fields the client did not send. When a fallback model answers, the fallback model's own defaults apply.
- The admin console's model settings show and edit the defaults.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `control-plane/llm-gateway`: Sampling Defaults Setting.
- `control-plane/admin-console`: Sampling Defaults Administration.

## Impact

Depends on `add-model-fallback-and-effort` (per-model settings, the model settings API and the Connections view). Adds one nullable column to `llm_models`. No client change. Out of scope: separate parameter sets per mode (thinking and non-thinking); an administrator stores the set that matches how the model is served.
