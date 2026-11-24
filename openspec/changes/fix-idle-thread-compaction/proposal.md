# Proposal

## Why

On 2026-11-24 the product owner reported that a Thread's status bar shows "Context ~100%" and that automatic compaction never happens. Only "Compact now" brings the usage down.

The runtime checks the compaction trigger only before it sends a model request (Context Compaction, Working Context Budget). Nothing checks it after the model's final answer. A Run whose last request was just below the trigger therefore ends with the Thread at or above it: the final answer is added, and provider usage from that last request can raise the calibrated estimate. The Thread then waits in that state until the user writes again. The context indicator warns, and "Compact now" is the only way down. The diagnosis is in design.md.

## What Changes

- When a top-level Run has its final answer and the Thread's estimated input, measured as its next request would be, is at or above the compaction trigger, the runtime compacts the Thread before the Run ends. It uses the same estimate, calibration, trigger, target and procedure as the check before a request.
- A failed compaction at that point leaves the answer and the Run's success unchanged. It is recorded in the Activity log, as any failed compaction is.

Out of scope:
- The trigger, the target and the Working Context Budget themselves (improve-skowork-and-knowledge-control).
- The context indicator when the model's limits are not known yet. It uses the 32,768-token fallback from Model-Aware Budget Refresh, which can show about 100% while the client is not connected (design.md, follow-up).
- Child Runs. Their history ends with them.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/agent-runtime`: adds Compaction After The Final Answer.

## Impact

- **Roadmap:** Hardening. It depends on add-agent-runtime and is compatible with improve-skowork-and-knowledge-control, which owns the trigger and the budget.
- **Code:** `client/src/runtime/runtime.cpp` (the end of a model turn).
- **Data:** none. The compaction record and Activity entry are the existing ones.
- **Cost:** one summarizer request at the end of a Run that ends above the trigger. The next Run's first request would otherwise make the same request.
