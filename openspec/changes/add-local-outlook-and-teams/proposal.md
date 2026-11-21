# Proposal

## Why

The Graph route of client/mail-calendar cannot be used yet: the organization's Entra app registration is not configured for Skowork (add-mail-calendar task 6.4), and conditional access may block it for a long time. Every employee laptop already runs classic Outlook and the new Microsoft Teams client, signed in to the user's own account. Both keep the user's mail, calendar and chats on the device. The user asked (2026-11-21) to use them so that Skowork can:

1. fetch mail and calendar from the local Outlook app and chats from the local Teams storage;
2. show a Calendar in Skowork;
3. add the fetched data to knowledge after filtering, categorization and triage, as for the other Sources;
4. tell the user what they should know in a daily report;
5. put what the user must act on in the Agenda, with a prepared draft built from all knowledge that the user can edit and send through Outlook.

The user named three open-source projects as references: mbe24/zaungast (reads the new Teams cache), and jagarlamudisrinath/outlook-mcp and lihaokun/outlook-mcp (Outlook object model). No code is taken from them; design.md records what was learnt.

## What Changes

- **Local Outlook backend for client/mail-calendar.** A new policy key policy.mail.backend = auto | graph | outlook (default auto). With auto, the client uses Graph when policy.mail.oauth is complete and otherwise the local classic Outlook of the signed-in Windows user. Every existing mail and calendar behaviour (tools, risk classes, the send approval floor, labels, exclusions, sync window, sync, event feed, triage, Sources, Triggers, Agenda intake) works the same on both backends. Sending goes through Outlook, so the message leaves from the user's own Outlook profile and is filed in its Sent Items.
- **New capability client/teams-chats.** Read-only access to the chats, channel posts and meeting chats that the new Microsoft Teams client keeps in the user's profile. The client copies the cache, reads the copy, keeps a local copy of the messages in the sync window, and provides the tools teams.search and teams.read. Its changes feed knowledge, the Agenda and the daily report. Skowork never writes to Teams or its cache.
- **New capability client/calendar-view.** A Calendar page with a day, a work week and an agenda list from the local calendar copy. From an event the user can open the meeting's Thread, ask Skowork about the event, or join an online meeting.
- **New capability client/daily-report.** Once each working day, at a time the user picks, the client writes a Daily report Thread. It lists today's meetings, what arrived since the last report that the user should know (mail, Teams chats), and the open, due and overdue Agenda items, with Citations. The report is written by a read-only Run.
- **client/knowledge-graph:** a new Source kind `teams` for Teams chats, with the same filtering, salience and common-sense triage as mail.
- **client/agenda:** Teams chats become an Agenda source. A prepared draft reply can be edited in the Agenda item and sent by the user through the mail backend, or opened in Outlook as a draft.
- **Project context:** the hard constraint "Email and calendar access uses Microsoft Graph only" is replaced; the glossary gains Teams chat, Daily report and the Source kind `teams`.

Out of scope:
- New Outlook (olk.exe) and Outlook on the web: they have no local object model. On such devices only the Graph backend works.
- Classic Teams (the Electron client), which Microsoft retired in 2025.
- Writing to Teams: sending chat messages, reactions or presence. Replies to Teams messages are prepared as mail or copied by the user.
- Shared and delegated mailboxes and shared calendars, as in add-mail-calendar.
- Teams files, recordings and call history.
- Outlook rules, categories management, contacts and tasks.

## Capabilities

### New Capabilities
- `client/teams-chats`: reading the local Teams cache, the local Teams copy, exclusions, tools and the event feed for knowledge, the Agenda and the daily report
- `client/calendar-view`: the Calendar page
- `client/daily-report`: the Daily report Thread

### Modified Capabilities
- `client/mail-calendar`: backend selection and the local Outlook backend (the Graph-only requirement of add-mail-calendar is narrowed to the Graph backend in that change)
- `client/knowledge-graph`: Source kind `teams`
- `client/agenda`: Teams chats as a source; sending or opening a prepared draft

## Impact

- **Roadmap:** change 52 (Productivity).
- **Depends on:**
  - add-mail-calendar (client/mail-calendar, not yet archived; this change narrows its Graph-only requirement in place)
  - add-knowledge-graph, add-common-sense-knowledge-triage and add-knowledge-salience (Sources, triage)
  - add-agenda (Agenda items, preparation)
  - improve-proactive-durable-work (event triage of mail and calendar)
  - add-native-ui (the Calendar page and settings sections)
- **Other capabilities referenced:** client/policy-enforcement (risk classes, approvals), client/agent-runtime (untrusted content and provenance), client/work-inbox (Threads, Action items, notifications), client/desktop-host (background deferral), client/local-storage (encryption and forget operations).
- **New code:** the Outlook object-model adapter (a COM client on its own apartment thread), a LevelDB, Snappy, IndexedDB and V8 value reader for the Teams cache, the Teams store and tools, the Calendar page, the daily report service.
- **No new infrastructure.** No network connection is added: Outlook and Teams do their own networking.
- **Testing:** the development machine has no Outlook or Teams. The adapters run in unit tests against fakes and generated cache files; tasks 1.1 to 1.3 validate them on an organization laptop before a pilot.
- **Security and privacy review:** the Outlook object-model guard, reading another application's cache, and the daily report's content.
