# Proposal

## Why

On 2026-11-10 the product owner reported four problems in the desktop client:

1. **A repeated answer.** When a Run thinks before it answers, the last answer appears twice in the chat.
2. **Unrelated threads in the chat.** A Thread shows a bare list of links to other Threads between its turns. The list comes from knowledge search results, whether or not the answer used them.
3. **Freezes.** The desktop UI sometimes freezes, usually while other work runs. Every streamed token redraws work on the UI thread, and periodic work runs there too.
4. **Copy.** The user cannot copy part of a message with Ctrl+C. Only the whole turn can be copied, and live updates clear a selection.

The code reading in design.md Context confirms each cause, with corrections for problems 3 and 4.

## What Changes

- The runtime sends every stored Run event live, including reasoning, so the feed never sees a sequence gap for a Run that thinks.
- The Thread feed shows a streamed answer once. The stored answer replaces the streamed text of its Run.
- The Thread feed lists only the knowledge passages that the answer cites, under the answer, as a numbered list. The agent is asked to cite passages with their numbers in square brackets.
- Passage numbers are unique within a Run, so a number names one passage.
- The Thread feed repaints at most 30 times per second and only for the Thread on screen. Live updates keep the selection and the reading position of unchanged messages.
- Pressing on message text gives that message keyboard focus, so Ctrl+C copies the selection. Live updates clear a selection only when that message's own text changes.
- The desktop UI does a bounded amount of work per task. Periodic work and background requests run apart from UI requests, so they do not block the UI thread or each other.

Out of scope:
- Model fallback, reasoning effort and the prompt cache. These belong to fix-long-thread-model-calls and add-model-fallback-and-effort.
- Changing how knowledge search ranks or chooses passages (add-knowledge-graph, improve-skowork-and-knowledge-control).
- A structured citations field in model output. It would need a model API change.
- Show work details defaults and the Activity rail layout (hide-work-details, improve-desktop-look).
- Redesigning the feed as a retained-mode renderer.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/work-inbox`: Work Details Visibility and Message Text Selection are modified. Cited Sources In The Feed and Live Feed Updates are added.
- `client/agent-runtime`: Run Event Stream and Knowledge Citation Markers are added.
- `client/knowledge-graph`: Citation Numbers Per Run is added.
- `client/desktop-host`: UI Thread Work Limit and Background Work Off The UI Thread are added.

## Impact

- **Roadmap:** 39 [Hardening] (openspec/config.yaml).
- **Dependencies:** This change MODIFIES requirements that are not yet in the accepted specs. It must be archived after these in-flight changes, in this order:
  - add-client-foundation (creates client/desktop-host);
  - add-agent-runtime and add-work-inbox (Streaming Thread Feed, run events);
  - add-knowledge-graph (knowledge.search, Search result citation);
  - add-native-ui (the native feed and its design of 30 updates per second);
  - hide-work-details, then improve-desktop-look (Work Details Visibility, Message Text Selection);
  - fix-long-thread-model-calls (Stable Prompt Prefix, used by Knowledge Citation Markers).
- **Compatibility:** no change to the control plane, the gateway or the wire protocol. The runtime and the window ship in one client. improve-skowork-and-knowledge-control renames product text but keeps citation identifiers, so this change does not conflict with it.
- **Code (for the apply step):**
  - client/src/runtime: reasoning events, citation numbering and the system prompt;
  - client/src/knowledge: passage numbers per Run;
  - client/src/shell and client/src/ui: feed folding, citation list, keyed updates, focus and Ctrl+C;
  - client/src/app: event coalescing, scheduler and file-watch work, two thread pools.
- **Data:** no schema change. The Run event log is unchanged; reasoning is stored as before.
