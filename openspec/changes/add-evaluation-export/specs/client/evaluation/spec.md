# Spec Delta

## ADDED Requirements

### Requirement: Evaluation Setting
The client SHALL provide evaluation export and replay only when policy.evaluation.export is on (values off | on; default off). The evaluation methods SHALL be callable only through the signed-in user's local API pipe. They MUST NOT be exposed to the model as Tools, and MUST NOT be reachable from Triggers, Remote tasks, MCP servers or API connectors. While the setting is off, every evaluation method SHALL refuse with the reason not_enabled and SHALL write nothing. While it is on, Settings SHALL state that evaluation export is on.

#### Scenario: Visible when on
- **WHEN** the Policy bundle turns policy.evaluation.export on
- **THEN** Settings shows that evaluation export is on

#### Scenario: Setting off
- **WHEN** policy.evaluation.export is off and a developer calls the export method on the local pipe
- **THEN** the call fails with not_enabled and no folder or file is created

#### Scenario: Agent cannot export
- **WHEN** policy.evaluation.export is on and a Run asks for a tool that would export mail
- **THEN** no such Tool exists in the Run's tool catalog

### Requirement: User Decision Log
The client SHALL keep, in its encrypted local storage, a log of each decision the user makes about a result Skowork produced, whatever policy.evaluation.export is set to. The decisions logged are:
- Dismiss, No action needed and Start work on a triage suggestion;
- marking an Agenda item done, dismissing it or editing it;
- editing, deleting or marking the importance of a fact;
- renaming a meeting speaker;
- adding a meeting vocabulary term, and choosing to transcribe a meeting again.

Each entry SHALL record:
- the time;
- the kind of decision;
- the item and the result it concerns;
- the value before and after;
- the identifier of the instructions and settings that produced the result.

The log SHALL NOT leave the device except through an evaluation export. Entries SHALL be removed when their item is forgotten or deleted, and once they are older than 365 days.

#### Scenario: Decision recorded
- **WHEN** the user chooses No action needed on a triage suggestion that said action is required
- **THEN** the log holds an entry with the time, the kind no_action, the item, the previous action required and the triage instruction identifier

#### Scenario: Later decision does not erase an earlier one
- **WHEN** the user dismisses a suggestion, and triage later re-evaluates the same item and the user chooses Start work
- **THEN** the log holds both decisions with their times

#### Scenario: Forgotten item
- **WHEN** the user forgets a mail in the Knowledge view
- **THEN** the log entries for that mail are removed

### Requirement: Export Of Items, Verdicts And Decisions
On request, the client SHALL export to a folder named by the caller:
- every stored mail, Teams chat message and calendar event received or starting in a chosen time window, for the Sources chosen;
- for each item, its stored metadata and text: sender, recipients (to and cc), date, subject, folder, conversation, and whether the user replied or sent the item;
- for each item, the results Skowork produced: triage outcome, action, category and reason; Agenda items with their quotes and states; and knowledge facts with their statement and status, together with the extracted relations supporting them and those relations' salience and durability;
- for each item, the decisions the user made about it: all its entries in the User Decision Log, and, for decisions made before the log existed, the state stored with the result (Dismiss, No action needed and Start work on a triage suggestion; done, dismissed and edited Agenda items; edited, deleted and importance-marked facts), with its time where the client recorded one, and marked as having no time otherwise.

Items that client/mail-calendar withholds SHALL NOT be exported and SHALL only be counted. Items it excludes are never stored, so they cannot be exported. The export SHALL NOT change any stored item, result or decision.

#### Scenario: Window and Sources
- **WHEN** the developer exports mail and Teams for 2026-09-01 to 2026-11-26
- **THEN** the folder holds one record for each stored mail and Teams chat message in that window, and no calendar events

#### Scenario: Withheld mail
- **WHEN** a mail in the window is withheld because of its sensitivity label
- **THEN** it is not in the export, and the manifest counts it as withheld without giving its subject or sender

#### Scenario: User decision included
- **WHEN** the user chose No action needed on a triage suggestion for a mail in the window
- **THEN** that mail's record holds the triage result and the decision no_action, with the time it was made if the client recorded it

#### Scenario: Speaker named by the user
- **WHEN** the user renamed a meeting's "Speaker 2" to an attendee after this change was installed
- **THEN** the meeting record marks that name as given by the user, with the time, and marks names the client chose itself as automatic

### Requirement: Export Of Meetings
On request, the client SHALL export each meeting captured in the time window with:
- its transcript segments (track, times, speaker, text and the unclear and echo marks);
- its speakers, and the names the user gave them;
- its attendees and its linked calendar event;
- its vocabulary and its detected or chosen language.

When the caller asks for audio and the meeting's audio is still kept, the client SHALL also write each track as an unencrypted WAV file. Without that request it SHALL write no audio. A meeting whose audio is no longer kept SHALL be exported without audio, and this SHALL be noted in its record.

#### Scenario: Audio requested
- **WHEN** the developer exports meetings with audio and a meeting still has both tracks
- **THEN** the folder holds that meeting's transcript record and one WAV file for each track

#### Scenario: Audio already deleted
- **WHEN** a meeting's audio retention has passed
- **THEN** the meeting is exported with its transcript and speakers, and its record states that audio is unavailable

### Requirement: Export Manifest
Every export SHALL include a manifest stating:
- the time window, the Sources chosen and whether audio was requested;
- the client version;
- the number of records written and the number of items withheld, per Source;
- for each step whose results are exported, an identifier of the instructions and settings in effect.

#### Scenario: Results tied to instructions
- **WHEN** an export holds triage results
- **THEN** the manifest names the identifier of the triage instructions and settings the client currently uses

### Requirement: Replay Without Side Effects
On request, the client SHALL replay one step over the items of an export: triage, Agenda detection, knowledge extraction, speaker diarization and naming, or transcription. It SHALL write the results into that export folder, together with the identifier of the instructions and settings used.

A replay MUST NOT change stored items, results, decisions, Threads, Inbox entries, Agenda items, the knowledge graph or meeting transcripts. It MUST NOT raise notifications, and it MUST NOT count against the call budgets of the live features. Model calls during a replay SHALL use the same Model role as the live step and SHALL go through the control-plane LLM gateway.

#### Scenario: Triage replay
- **WHEN** the developer replays triage over an export of 200 mails
- **THEN** the export folder holds 200 new triage results, and the Inbox, Agenda and Daily report are unchanged

#### Scenario: Transcription replay without audio
- **WHEN** the developer replays transcription for a meeting that was exported without audio
- **THEN** that meeting is skipped and reported as having no audio

### Requirement: Replay Variants
A replay SHALL accept a variant that overrides named instructions and named settings of the replayed step, for that replay only. A variant naming an instruction or setting that does not exist, or giving a setting a value outside its allowed range, SHALL be rejected before any item is replayed. Live features MUST NOT read variants.

#### Scenario: Unknown name
- **WHEN** a variant overrides "triage.instructons"
- **THEN** the replay is rejected and names the unknown key

#### Scenario: Live behaviour unaffected
- **WHEN** a replay with a variant is running and new mail arrives
- **THEN** the new mail is triaged with the live instructions and settings

### Requirement: Replay Limits And Progress
A replay SHALL process at most the number of items the caller gives (default 200, maximum 2000). It SHALL report its progress on request and SHALL stop when cancelled, keeping the results written so far. Only one replay SHALL run at a time.

#### Scenario: Cancel
- **WHEN** the developer cancels a replay after 50 of 200 items
- **THEN** the replay stops, and the 50 results written so far remain in the export folder
