# Design

## Context

- The runtime caps each tool result at policy.llm.max_tool_result_kb (default 64 KB). Above it, the result goes to a Thread attachment and the model gets a 4 KB preview and a reference.
- The context window of a role comes from the gateway's model list. Without one, the client assumes 32,768 tokens, and so does the gateway for a model with no stored settings.
- The compaction qualification of 2026-11-17 (improve-skowork-and-knowledge-control) recorded `main` at 32,768 tokens: input budget 24,576, compaction trigger 22,118.
- Token estimate: about three bytes per token for ASCII text, one token per non-ASCII byte, scaled by the usage calibration factor. 64 KB of plain ASCII text is about 21,800 tokens; Czech text is more.
- Compaction keeps the most recent messages (6 by default) and unresolved tool calls, so a response's own tool results are never compacted before the next model call.

## Decisions

### D1: Cap one result at a quarter of the context budget

Cap = min(policy.llm.max_tool_result_kb, 25% of the context window of the role the next request uses), compared in estimated tokens. For a 32,768-token window that is 8,192 tokens (about 24 KB of ASCII text). For a 262,144-token window the policy cap of 64 KB stays the limit.

Alternatives considered:
- A fixed KB cap per role, set by a platform-admin: needs new settings and still has to be kept in step with the model.
- A share of the input budget instead of the window: more precise, but the input budget depends on the output reserve and calibration. The window is the number the gateway reports, and a quarter of it leaves room either way.
- Keep 64 KB and rely on compaction: fails with parallel results, as shown in the proposal.

### D2: Cap one response's results together at half of the context budget

After each result is capped (D1), the runtime adds up the estimated tokens of all results of one model response that complete together. Calls that complete later, after an Approval or a Question, are counted with the calls that complete with them. Above 50% of the window, it offloads results largest first, ties in the order the model returned them, until the rest fits. Offloaded results get the ordinary preview and reference.

Alternatives considered:
- Lower policy.llm.max_parallel_tools for small windows: loses the speed-up for small results, which are the common case.
- No total cap: four results of just under 8,192 tokens each would still fill the window.

### D3: Measure in estimated tokens, not bytes

The cap is compared with the runtime's calibrated estimate (Complete Request Accounting), the same one compaction uses, so both agree on what fits. Text with many non-ASCII bytes is offloaded sooner, which is intended.

## Risks

- [More results are offloaded on small windows, so the model needs more files.read calls on attachments, and Runs reach model_calls sooner] -> The preview and reference stay as they are. The per-result cap only changes where a quarter of the window is below 64 KB of ASCII text (about 21,800 tokens), that is for windows below about 87,000 tokens; larger windows behave as today, apart from the per-response total.
- [The estimate is off for an unusual tokenizer] -> The calibration factor corrects it after the first requests. A quarter leaves a wide margin.
- [A role switch within a Run (`vision` for images) changes the window] -> The cap uses the role of the next request, the same role compaction uses.
