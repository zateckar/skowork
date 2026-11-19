# Proposal

## Why

Agent work is long-running and often finishes or blocks while the user is busy elsewhere. Users already know how to triage email. An inbox of Threads, with Unread and Action queues, lets them pick up results and unblock the agent without learning a new mental model. This UX has been proven in pizza-bot with more than 2,000 users. We re-specify it here and close its known gaps.

## What Changes

- Add the work inbox UI:
  - Threads with automatic titles, previews, pinning, folders and origin badges.
  - The smart views Unread, Action, Running, All and Archive, plus folders that work independently of those views.
  - Email-like triage: open to mark read, mark unread, archive and restore, delete with confirmation.
- Add the three-pane layout:
  - a Thread list;
  - a Thread feed with streaming text and collapsible tool-call cards;
  - an Activity rail showing grouped subagent Runs, with their Run states and lazily loaded transcripts.
- Add Action item presentation:
  - Approval cards with approve, edit arguments, reject and respond-instead. Each card shows the agent's stated rationale and the provenance of the untrusted inputs behind it.
  - Question cards for `ask_user` and for client-raised Questions.
  - Standalone Action items that client capabilities raise without a Run (for example connect mailbox, Remote task acceptance).
- Add the composer:
  - attachments and per-Thread drafts;
  - `/` skill invocation;
  - queue-or-steer while a Run is active, and Stop;
  - fork from any message.
- Add error banners with a remedy, Windows toast notifications, full-text search with jump-to-message, keyboard shortcuts, a status bar, the settings structure, cs/de/en localization and accessibility.
- Fixes for pizza-bot gaps:
  - Archive state and mark-unread.
  - A real Running view across all Threads.
  - The approval rationale and provenance shown on Approval cards.
  - Answerable agent questions.
  - Links from trigger-created Threads back to their Trigger and Occurrence.

Out of scope:
- Run semantics such as queuing, steering, cancellation and interrupts (client/agent-runtime).
- Approval decisions (client/policy-enforcement).
- The Automations editor (client/scheduler).
- Skill and knowledge management screens, beyond their entry points in settings.

## Capabilities

### New Capabilities
- `client/work-inbox`: Thread list and smart views, triage actions, three-pane layout, Action item cards, composer, notifications, search, shortcuts, status bar, settings structure, localization and accessibility

### Modified Capabilities
- None.

## Impact

- **Roadmap:** change 4 of 13 (MVP). It depends on:
  - add-client-foundation (window, local storage, the control-channel connection state);
  - add-agent-runtime (Threads, Run states, interrupts, client-raised Questions, steering, manual compaction, provenance and rationale, error codes);
  - add-policy-and-sandbox (Approval cards present decisions from client/policy-enforcement).
- **Forward references:**
  - Remote task and meeting origins are produced by add-scheduled-and-remote-tasks (10) and add-meeting-capture (13).
  - The Connections section and the status-bar MCP health come from client/mcp-client in add-core-tools-and-mcp (5). The Connections section also shows agent-browser status from add-browser-use (6).
  - Spec scenarios use client/computer-use (change 8) takeover Questions and the client/mail-calendar (change 11) Connect mailbox item as examples of client-raised and standalone Action items.
  - The Mail and calendar, Knowledge sources, Meetings and Privacy sections are filled by changes 7, 11, 12 and 13.
- **New code:**
  - the `client/ui/` single-page app (inbox, Thread feed, Activity rail, composer, settings shell);
  - host-side view-model and notification services in `client/`.
- **Later dependants:** add-scheduled-and-remote-tasks (origin links), add-skills (`/` invocation), and add-knowledge-graph (the Knowledge sources settings page).
