# Proposal

## Why

Building a Local app still fails with TIMEOUT on gamma after `fix-long-thread-model-calls` was deployed. A probe through Traefik on 2026-11-20 showed why. The organization LLM API does not stream tool-call arguments. While the model writes a large `files.write` argument, the upstream sends nothing. In the probe, about 15 KB of reasoning arrived within 3.5 s, then nothing for 230 s, and the gateway's idle limit cut the stream before the tool call arrived. The gateway's keep-alives did reach the client every 15 s.

The silence lasts as long as the model needs to write the whole argument, about 79 tokens/s on `main`. A 20,000-token file is about 250 s of silence. The limits on the path cannot be raised past that: APIM and other components between the client and the model allow at most 240 s. The gateway's first-byte limit is 180 s and its idle limit is 230 s.

## What Changes

- The system prompt of every Run and subagent Run tells the model to keep each tool call's arguments under about 6,000 tokens (about 20,000 characters, roughly 400 lines of code). It also tells the model to split larger content over several tool calls, for example by writing separate files, or by writing the first part of a file and appending the rest.
- `files.write` gets an optional `append` flag that adds the content to the end of the file, creating the file if it does not exist. This makes "write the first part, then append" possible for a single large file.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `client/agent-runtime`: Tool Call Size Instructions (added).
- `client/core-tools`: File Append (added).

## Impact

Model-facing text in the runtime's system prompt and the `files.write` tool definition, and the write path in `client/src/tools/files.cpp`. The system prompt stays byte-stable across Runs (Stable Prompt Prefix): the sentence is constant. No change to the control plane, the gateway limits or the UI.

Out of scope: enforcing the bound by rejecting large tool calls (the arguments arrive only after the silence, so rejecting them saves nothing), and the upstream fix of streaming tool-call arguments, which belongs to the LLM API owner.
