# Proposal

## Why

files.edit fails unless its text span occurs exactly once. That protects against editing the wrong place, but it makes a rename across a file expensive: the model has to make one call per occurrence, each with enough context to be unique, or rewrite the whole file with files.write. Rewriting is what `bound-tool-call-size` tries to avoid, because a large tool call argument can exceed the gateway's silence limits. Renames are common when the agent builds or changes a Local app.

## What Changes

- files.edit gets an optional `replace_all` flag. When it is true, every occurrence of the text span is replaced, the call fails only when there is none, and the result states how many occurrences were replaced.
- Without the flag, files.edit behaves as today.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `client/core-tools`: File Write Tools (modified), Edit All Occurrences (added).

## Impact

- Roadmap: Hardening, after improve-transcript-language. Depends on add-core-tools-and-mcp (File Write Tools). Complements `bound-tool-call-size`.
- Code: the files.edit definition and edit path (`client/src/tools/files.cpp`). The tool definition changes once, so the system prompt prefix changes once (Stable Prompt Prefix).

Out of scope: replacing the Nth occurrence, regular-expression replacement, and edits across several files in one call.
