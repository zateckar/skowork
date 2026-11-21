# Design

## Context

The model streams reasoning and text deltas, but the organization LLM API (vLLM behind LiteLLM) returns a tool call only once all its arguments are generated. Until then the upstream is silent. The gateway sends keep-alives to the client while it waits (control-plane/llm-gateway Keep-Alive Between Events). The silence is still bounded by the gateway's first-byte limit (180 s) and idle limit (230 s), and by APIM and other components on the path, which allow at most 240 s. A tool call whose arguments take longer to generate than these limits fails with TIMEOUT, or is cut off as truncated.

Probe of 2026-11-20 against gamma, through Traefik and CrowdSec:

- A ~300-line HTML file in one `files_write` call: 133 s of silence, then the call arrived; 8 keep-alives reached the client.
- A ~900-line file requested with low reasoning effort: 15 KB of reasoning in 3.5 s, then 230 s of silence, cut by the gateway idle limit before the tool call arrived.
- Observed generation speed on `main`: about 79 tokens/s.

## Decisions

### D1: Bound each tool call at about 6,000 tokens, by instruction

The bound has to fit inside the shortest limit, the gateway's 180 s first-byte limit, with room for:

- prefill of a long Thread (tens of seconds);
- reasoning before the call;
- a slower model under load (60 tokens/s instead of 79).

120 s of generation at 60 tokens/s is 7,200 tokens. 6,000 tokens leaves a margin, and at 79 tokens/s it takes about 76 s. Models estimate characters and lines better than tokens, so the instruction states the bound in all three: about 6,000 tokens, about 20,000 characters, roughly 400 lines of code.

The bound is an instruction, not a check. The arguments reach the client only after the silence, so rejecting them would not avoid the timeout.

Sentence, appended to the system prompt of every Run and subagent Run, after the untrusted-data instructions:

> Keep the arguments of each tool call under about 6,000 tokens (about 20,000 characters, roughly 400 lines of code): the model server sends nothing while you write a tool call, and a longer one exceeds its time limit. Split larger content over several tool calls, for example several smaller files, or write the first part of a file with files.write and add each further part with files.write and append set to true.

It is a constant, so the system prompt stays byte-stable across Runs (Stable Prompt Prefix).

### D2: files.write append

Splitting into several files suits most Local apps (HTML, CSS and JS apart), but a single large file needs a way to be written in parts. `files.edit` cannot append without a unique anchor, so `files.write` gets an optional boolean `append`:

- When `append` is true, the content is added to the end of the existing file; a missing file is created.
- The write stays atomic: the client reads the current content and writes it back with the addition through the same atomic write and path lock. The 8 MB file read limit of `files.edit` applies.

## Risks

- The model may not follow the instruction, or may misjudge the size. The output-limit continuation (`handle-output-limit`) already asks it to split long content after a cut-off. A TIMEOUT Run can be retried. Verification with a real-gateway Local app request is task 3.1.
- 6,000 tokens is an estimate based on one probe. If generation slows down, the bound has to be lowered in this sentence.
