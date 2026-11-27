# Proposal

## Why

A review of the open-source CUI component library (MIT; C99, immediate mode, own renderer) against Skowork showed that Skowork's specs already cover the larger conversation pieces (streamed feed, tool cards, Approval and Question cards, composer with queue and steer, Markdown, notifications), and better: CUI has no approval flow and no screen reader support, so its code is not adopted. Six smaller interaction patterns from it would make Skowork's feedback clearer:

1. While an answer streams, an unfinished `**`, `` ` `` or link at the end of the text shows its raw markup and then jumps into formatting. Answers flicker.
2. A Run says what it is doing ("Reading files") but not for how long, and a finished tool card or reasoning block does not say how long it took. Tool results that are JSON are shown as one long line.
3. Everything is reachable by shortcuts or navigation, but there is no single searchable list of commands and Threads.
4. A long multi-step Run shows only its current step. The user cannot see the agent's plan or how far it is.
5. The context indicator shows one percentage. It does not say what fills the context, and it has only one warning level before compaction.
6. Small rules: Send is enabled on an empty draft, a Copy button gives no visible result, every avatar has the same colour, and wrapped labels can end with one word alone on the last line.

## What Changes

- **Stable streaming Markdown.** While an answer streams, an unfinished emphasis, strikethrough, code span or link at the end of the text is shown as if it were closed.
- **Elapsed time.** The status line of an active Run shows how long its current step has been running. A tool card shows its duration (and the time so far while it runs). A reasoning block says "Thinking · 8 s" while it streams and "Thought for 8 s" when it is stored. A tool result that is JSON is shown indented.
- **Command palette.** Ctrl+K opens a searchable list of commands and Threads, ranked by how well they match, operable by keyboard alone.
- **Run plan.** A new runtime tool `update_plan` lets the agent publish an ordered list of steps with their status. The feed shows the latest Run plan of a top-level Run as one card, also with work details hidden.
- **Context breakdown.** The context indicator turns to the error colour at 90 % and describes what fills the context: instructions, tool definitions, conversation and free space.
- **Small rules.** Send, Queue and Steer are disabled while the draft is empty and has no attachments, and while a Run is active Stop is then the primary button. Copy buttons show their result on the button. Each person's avatar has a stable colour of their own. Wrapped interface text does not end with one word alone on its last line.

Already covered and not changed: dictation never sends (client/dictation Dictation in Composers), desktop notifications are only for background Runs (Desktop Notifications), Stop cancels the Run (Queue Or Steer While Running).

Out of scope:
- Taking CUI code or its renderer; animations (shimmer, springs) and reduced-motion handling, since nothing here animates.
- Response voting, regenerate, prompt suggestions, voice components and a notification centre.
- A plan for Child Runs in the feed (their transcripts stay in the Activity rail), plan editing by the user, and plans in Remote task results.
- Balancing the lines of message text (Markdown answers and Chat messages), which keeps its own text layout for selection.
- Deterministic soak tests and per-frame draw fingerprints.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/agent-runtime`: the `update_plan` tool (Run plan) and the stored duration of reasoning.
- `client/work-inbox`: Markdown Rendering (streaming), Streaming Thread Feed (durations, JSON results), Composer (empty draft, Stop), Keyboard Shortcuts (Ctrl+K), Status Bar (context breakdown and error level); new requirements for step elapsed time, the command palette, the Run plan card, button feedback, avatar colours and balanced line wrapping.

## Impact

- **Roadmap:** proposed position 57 [Productivity], after improve-remaining-views.
- **Depends on:** add-work-inbox and add-native-ui (archived: feed, composer, status bar, toolkit); add-agent-runtime (archived: tool loop, core tools); show-generation-speed (in flight: the one-second feed timer and the model call timing this change reuses for reasoning); fix-feed-live-updates (in flight: Live Feed Updates and the status line of Work Details Visibility, which this change does not modify).
- **Forward references:** none.
- **Glossary:** adds "Run plan" and the tool id `update_plan`.
- **Code:** client runtime (`update_plan`, reasoning duration, context parts), shell feed, composer, status bar, a new command palette view, UI toolkit (Markdown preprocessing, text layout, avatar palette), en/cs/de strings; control-plane built-in tool catalogue (`update_plan`). No policy key, storage schema or gateway change.
