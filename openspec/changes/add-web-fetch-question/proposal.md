# Proposal

## Why

web.fetch returns a page's readable text. Pages are long: documentation, intranet articles and reports are often tens of kilobytes, of which the agent needs one fact. The whole text enters the conversation, or a preview and an attachment when it is above the tool-result cap, and the model then reads it in parts. On a 32,768-token window this crowds out the rest of the work and causes compaction.

Another agent harness answers this differently: its fetch tool takes a prompt, has a small model answer it from the converted page, and returns only the answer.

## What Changes

- web.fetch accepts an optional `question`. With a question, the client sends the page's readable text and the question to the `small` model role, without tools, and returns the answer instead of the page text.
- The answer stays untrusted content with the fetched URL as provenance.
- When the page text does not fit the `small` role's context budget, it is cut, and the result says how much of the page the answer covers.
- When the `small` role is not available or its call fails, web.fetch returns the page text as it would without a question, with a note.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `client/core-tools`: Web Fetch Question (added), Web Fetch Question Fallback (added).

## Impact

- Roadmap: Hardening, after improve-transcript-language. Depends on add-core-tools-and-mcp (Web Fetch Tool), client/model-access (Model Roles, usage accounting) and client/agent-runtime (Untrusted Content Provenance).
- Code: the web.fetch definition and path (`client/src/tools/web.cpp`) and a `small`-role request from the tool. No control-plane change: the `small` role and its budget already exist.
- Cost: one `small` request per web.fetch with a question, counted to the Run like other model requests.

Out of scope: questions on `raw` fetches and on files saved to the quarantine folder; answering over several chunks of a page that does not fit (map-reduce); caching answers.
