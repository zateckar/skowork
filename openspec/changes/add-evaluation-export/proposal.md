# Proposal

## Why

Skowork has recorded and transcribed real meetings and collected real mail, Teams chats and calendar events on a developer's own instance. Quality problems are visible in that data: a 2½-month-old mail shown in the Inbox as new and needing action, speakers left as "Speaker N" even though the calendar names the attendees, triage batches that fail with cut-off or mismatched answers, and knowledge that repeats or keeps trivia. None of this can be measured today. The data is in encrypted stores that only the app can open. Triage, knowledge extraction, speaker naming and transcription have no labelled set and no metrics. The user's own decisions (Dismiss, No action needed, Start work, speaker renames, fact edits) are stored but never used to judge quality.

Before any of these features changes, we need a way to measure them against real data and to try alternative instructions and settings quickly. The user owns the data and decided on 2026-11-26 that a plaintext export to their own machine is acceptable, since the same mail and meetings are already in the organization's Microsoft cloud.

## What Changes

- Add a developer evaluation mode, off by default (policy.evaluation.export).
- Export, through the local API only: mail, Teams chats, calendar events and meetings for a chosen time window, together with what Skowork concluded about each item (triage, Agenda, knowledge facts, speakers) and what the user decided about it.
- Optionally export meeting audio as unencrypted WAV so transcription and diarization can be replayed.
- Replay one step (triage, Agenda detection, knowledge extraction, speaker diarization and naming, transcription) over an export, without changing anything the user sees. A replay can use a variant file that overrides named instructions and settings for that run only.
- Give every model instruction and tunable setting used by these steps a stable name, so a variant can override it and an export can record which version produced a result.
- Keep a local log of every decision the user makes about a result (Dismiss, No action needed, Start work, Agenda edits, fact edits, speaker renames), with its time. The log is kept for every user whether or not evaluation is on, so history builds up from now on for evaluation and for later per-user learning.
- Send daily quality counters with no content through telemetry (for example the share of triage suggestions dismissed, or speakers left unnamed). This shows how quality changes across all users and releases, not just on one developer's instance.
- Developer tooling (outside the client) to export, label, score and compare runs, and a recorded baseline for this instance.

## Capabilities

### New Capabilities

- `client/evaluation`: the evaluation setting, the user decision log, export of items, verdicts, decisions and meetings, the export manifest, side-effect-free replay with variants, and replay limits.

### Modified Capabilities

- `client/telemetry`: daily quality counters at the metadata level.

Otherwise none. Each replayed step keeps its owning capability (client/mail-calendar and client/event-triage for triage, client/agenda, client/knowledge-graph, client/meeting-capture). Their behaviour does not change; only where their instruction text and settings are kept changes, which is design, not behaviour.

## Impact

Roadmap: proposed as entry 53, after add-local-outlook-and-teams. It comes before the quality changes it measures. Those are planned as separate follow-up changes, in this order:
1. Inbox recency and common-sense triage.
2. Speaker naming from the calendar.
3. Transcription quality.
4. Mail and chat knowledge and presentation.
5. Per-user learning from the user's own decisions.

Depends on add-mail-calendar, add-local-outlook-and-teams, add-teams-triage, improve-triage-results, add-meeting-capture, improve-meeting-capture-quality, add-knowledge-graph and add-knowledge-salience, which are in flight. The quality counters add a requirement to client/telemetry, which add-telemetry-and-admin introduces and which is not archived yet (17 of 18 tasks done). That change must be archived before this one. Forward references: client/mail-calendar (mail filter, items withheld or excluded), client/event-triage, client/teams-chats. At archive, rebase on whichever of these are archived by then.

New Policy bundle top-level key `evaluation`, with the key policy.evaluation.export owned by client/evaluation. The glossary list of top-level keys in openspec/config.yaml is updated.

Data protection: the export writes personal data in plaintext to a folder the developer chooses. That is the developer's own decision for their own data. The feature is off by default, reachable only through the signed-in user's local pipe, and never by the agent, Tools, Triggers or Remote tasks. The project constraint "no real mailbox in automated tests" is unchanged: exports are used for manual evaluation only and are never committed or used by automated tests.

Out of scope:
- Any change to triage, Agenda, knowledge, diarization or transcription behaviour (each gets its own follow-up change, measured with this one).
- Per-user learning such as voice profiles or sender rules. The decision log is its input, but nothing reads the log to change live behaviour in this change.
- An in-app labelling view, including "right / wrong" buttons on results. These are planned with per-user learning.
- A "send this example to the Skowork team" action for users. It needs organizational data-protection approval.
- Quality counters in the admin console. Counters arrive in telemetry ingest; dashboards are later work.
- Upload of exports to the control plane or sharing them with a team.
- Automated tests on real data.
- Exporting Threads, Runs or Chats from user-messaging.
