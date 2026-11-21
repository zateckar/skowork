# Proposal

## Why

The runtime inserts a tool result into the conversation whole when it is at most policy.llm.max_tool_result_kb (default 64 KB). The cap does not depend on the model. The compaction qualification of 2026-11-17 recorded `main` with a context window of 32,768 tokens, an input budget of 24,576 and a compaction trigger at 22,118. A gateway model without stored settings also reports 32,768. With the runtime's token estimate (about three plain-ASCII bytes per token, one token per non-ASCII byte), a 64 KB result is about 22,000 tokens: one result fills the request.

It gets worse with parallel tool calls. Up to policy.llm.max_parallel_tools (default 4) results of one model response are appended together, and compaction never touches the most recent messages. Four results of 40–60 KB each can therefore exceed the window in a way that compaction cannot fix, and the Run fails with CONTEXT_LENGTH.

## What Changes

- Each tool result is capped at the smaller of policy.llm.max_tool_result_kb and a quarter of the context budget of the model role in use, measured in estimated tokens.
- The tool results of one model response together stay within half of that context budget. When they would exceed it, the largest are offloaded first.
- Offloading itself is unchanged: the result is stored as a Thread attachment, and the model gets a preview and a reference it can read in parts.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `client/agent-runtime`: Large Tool Results (modified), Tool Results Per Response (added).

## Impact

- Roadmap: Hardening, after improve-transcript-language. Depends on add-agent-runtime (Large Tool Results, Context Compaction) and on the role context windows reported by control-plane/llm-gateway.
- Code: the tool-result cap in the runtime's tool-result path (`client/src/runtime/runtime.cpp`). No control-plane or UI change. policy.llm.max_tool_result_kb keeps its meaning as an upper bound.
- The preview's shape is not changed here; `keep-result-preview-ends` changes it.

Out of scope: the files.read limit (policy.tools.files.max_read_kb) and the output capture limit of client/tool-sandbox, which are not model-context caps; changes to compaction.
