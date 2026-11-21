# Spec Delta

## MODIFIED Requirements

### Requirement: Large Tool Results
The runtime SHALL cap each tool result it inserts into the conversation at the smaller of policy.llm.max_tool_result_kb (default 64) and a quarter of the context budget of the model role in use, measured with the runtime's token estimate. A larger result is stored as a Thread attachment. The model receives a truncated preview plus a reference it can read in parts. This cap applies to every tool, including MCP tools.

#### Scenario: Large file read
- **WHEN** a tool returns 2 MB of text
- **THEN** the model receives a preview and an attachment reference instead of the full text

#### Scenario: Small context window
- **WHEN** the model role in use reports a context window of 32,768 tokens and a tool returns 40 KB of plain text, about 13,600 estimated tokens
- **THEN** the result is stored as a Thread attachment and the model receives a preview and a reference, because the result exceeds 8,192 tokens

#### Scenario: Large context window
- **WHEN** the model role in use reports a context window of 262,144 tokens and a tool returns 60 KB of plain text
- **THEN** the result is inserted in full, because it is below policy.llm.max_tool_result_kb and below a quarter of the context budget

## ADDED Requirements

### Requirement: Tool Results Per Response
The tool results of one model response SHALL together stay within half of the context budget of the model role in use, measured with the runtime's token estimate. When they would exceed it, the runtime SHALL offload the largest results first, as Large Tool Results does, until the rest fits. An offloaded result counts with its preview and reference, and a result no longer than its preview is not offloaded for this reason. Results are still appended in the order the model returned the calls.

#### Scenario: Parallel results together too large
- **WHEN** the model role in use reports a context window of 32,768 tokens and the four tool calls of one model response each return 20 KB of plain text, about 6,800 estimated tokens each
- **THEN** three of the results are offloaded with a preview and a reference, and the fourth is inserted in full, so that the results of that response, previews included, stay within 16,384 tokens
