# Proposal

## Why

Commitments with a future date reach the user through many channels: a manager's mail asking for a report by Friday, a meeting follow-up assigned to the user, an invitation that asks the user to present, a Chat message agreeing on a deadline. Skowork already reads these sources but does not turn them into a list of what the user has to do, and it does not help the user get ready. Users miss deadlines or start preparing too late.

## What Changes

- Add an Agenda: a list of Agenda items, each a dated commitment that clearly requires the user to do something in the future, with links to the sources it came from.
- Detect Agenda items from new mail and calendar items (client/mail-calendar), from meeting transcripts (client/meeting-capture) and from Chats (client/user-messaging), with the Agenda's own bounded intake that starts when the user enables it. Admit an item only when the user is clearly responsible, an action is required, the date is in the future and the evidence is quoted from the source. Everything else, including newsletters, broadcasts, FYI messages, vague or past dates, is left out.
- Merge the same commitment seen in several sources into one Agenda item.
- Prepare an Agenda item: a read-only Run gathers the relevant mail, meetings, files and knowledge, writes a brief with Citations and, where the item calls for one, a draft response, statement or conclusion for the user to edit. Nothing is sent or changed by preparation.
- Show the Agenda as an Inbox view, grouped by due date, with Prepare, Open, Done, Dismiss and Edit date, and remind the user before the due date.
- Opt in: off until the user enables it; automatic preparation is a separate opt-in.

## Capabilities

### New Capabilities

- `client/agenda`: Agenda item detection, admission rules, merging, lifecycle, preparation and reminders.

### Modified Capabilities

- `client/work-inbox`: the Agenda view and its controls.

## Impact

Roadmap: proposed after entry 47. Depends on `add-agent-runtime`, `add-policy-and-sandbox`, `add-work-inbox`, `add-mail-calendar`, `add-knowledge-graph`, `add-meeting-capture` and `add-user-messaging`. It does not depend on `improve-proactive-durable-work`: event-triage intake is not wired yet (its spike 1.1 is open), so the Agenda reads new mail and calendar items itself (design D1). Once event-triage lands, a later change may move the Agenda's mail and calendar intake onto it to share one model call per item. `client/meeting-capture`, `client/user-messaging`, `client/mail-calendar`, `client/local-storage` and `client/scheduler` are referenced, not modified.

Glossary: adds "Agenda" and "Agenda item"; the Policy bundle gains the top-level key `agenda` (owned by client/agenda). Both are added to `openspec/config.yaml` at archive.

Naming: "task" is not used for Agenda items because `tasks.*` Tools are scheduled agent jobs (client/scheduler).

Out of scope: Microsoft Teams chats and other chat providers (no Connector reads them today), Microsoft To Do or Planner sync, files and notes as detection sources, automatic sending, creating mail drafts or calendar entries without an explicit user action, automatic completion detection, delegation to colleagues, and team-wide agendas.
