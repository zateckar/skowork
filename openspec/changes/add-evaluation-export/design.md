# Design

## Context

A developer's own Skowork instance holds real data:
- Mail from 33 Outlook folders, Teams chats and calendar events, all in `app.db`.
- A knowledge graph in `kg.db`.
- Four captured meetings with both audio tracks, recorded 2026-11-24 and 2026-11-26, about 0.7, 12, 33 and 54 minutes long.

Both databases are SQLCipher with DPAPI-protected keys, so only a client running as that Windows user can read them.

Observed problems this change has to make measurable:
- **Inbox recency.** An Inbox entry takes its time from when the Thread was created, not from when the mail arrived. As a result, a mail from 2026-09-10 is listed as "5 hr ago", under Today, and marked as needing action.
- **Triage input.** Triage sees only From, Subject, date and body. It has no recipients, no "addressed to me" and no "already replied".
- **Triage failures.** About 100 triage batches failed with "unavailable", answers that were cut off, or results that did not match their event revision.
- **Speaker naming.** A speaker gets an attendee name only when there is exactly one cluster and one attendee.
- **Duplicated work.** Triage, Agenda detection and knowledge extraction each classify the same item separately, so their verdicts can disagree.
- **Unused feedback.** User decisions are stored but never used for measurement.

### Decision record

On 2026-11-26 the user (data owner and developer) chose:
- Both dev-time tuning and per-user learning, with evaluation first.
- Export of real data from the app, tuned outside it (option B), over measuring inside the app only (A) or synthetic data only (C).
- A plaintext export on their own OneDrive-synced machine is acceptable, because the mail and Teams meetings are already in the organization's Microsoft cloud.

## Goals / Non-Goals

Goals:
- Get real items, Skowork's results and the user's decisions out of the encrypted stores in a form a developer can label and score.
- Replay one step with changed instructions or settings, without rebuilding the client and without touching live state.
- Record a baseline for each step on this instance, so each follow-up change can show its improvement in numbers.

Non-goals:
- Improving any step. That is done in the follow-up changes listed in the proposal.
- A labelling UI.
- Automated tests on real data.
- Sharing exports.

Anti-goals, which would make this a failure even if it works:
- Live behaviour that depends on anything in an export or a variant.
- The agent being able to export data.
- An export that silently includes mail the user's filters exclude.

## Decisions

### D1. Export and replay through the local API pipe
`eval.export`, `eval.run`, `eval.status` and `eval.cancel` are offered on the pipe only, not to the window. `host/rpc.h` has only `ui` and `ui_and_pipe` today, so this change adds a third value, `pipe`. This is an internal registry detail: the local-API channel stays as client/desktop-host defines it, so no desktop-host delta is needed. The developer calls them with `dev/scripts/skowork-pipe.ps1` or a small wrapper, `dev/scripts/eval.ps1`. Long calls return a job id at once, and progress is read with `eval.status`.

Alternatives considered:
- A button in Settings. Rejected: it makes a developer feature visible to every user and adds UI work for no benefit.
- A standalone tool that unwraps `keys.bin` and reads the databases directly. Rejected:
  - it duplicates key handling, schema knowledge and migrations;
  - it bypasses the mail filter;
  - it cannot replay steps without linking most of the client.
- An agent Tool. Rejected: it would let a Run read the whole mailbox (an anti-goal).

### D2. Export format
The export goes into `<out_dir>/<YYYYMMDD-HHMMSS>/`:

```
manifest.json
mail.jsonl          one record per mail
teams.jsonl         one record per Teams chat message (with conversation id, type, topic, mentions)
calendar.jsonl      one record per calendar event (organizer, attendees, body text, series)
meetings.jsonl      one record per meeting (segments, speakers, renames, attendees, event id, vocabulary, language)
audio/<meeting id>/local.wav, remote.wav    16 kHz mono 16-bit PCM, only when audio was requested
```

Each item record has three parts:
- `item`: metadata and text as stored.
- `skowork`: triage, Agenda and facts, each tagged with the instruction id that produced it, where the stored row records one. Older rows carry `"unknown"`.
- `user`: decisions with their timestamps.

Records use stable ids, the same ids the client uses, so labels and replays can refer to them.

Field derivations decided after the builder check:
- **`user_replied`** is derived, not stored. It is true when a mail in the user's own sent folder (`own_mail_folder`) shares the item's `conversation_id` and is dated after it. The record marks the field `approximate`. Sent mail in the window is exported too, as items with `sent_by_user: true`, because it shows what the user acts on.
- **Withheld mail is counted, excluded mail is not.** Mail excluded by folder, category or user exclusion is dropped at sync (`mail_sync.cpp`) and never stored, so the export cannot count it. Only withheld rows are counted. Changing client/mail-calendar to count exclusions is out of scope.
- **Facts.** The unit for facts is the user-visible fact in `knowledge_facts` (statement, status, evidence). Each fact record also carries its supporting extracted relations, with their salience, durability and confidence from `relations`. Knowledge labels and metrics apply to facts. Relation fields explain the scores.
- **Decision times are best available:**
  - triage: `triage_items.updated_at` while the outcome still holds the decision, else none;
  - Agenda: the state change time where stored;
  - importance marks and tombstones: none.

  From this change on, the decision log (D8) is the source, and these fallbacks apply only to decisions made before it existed.
- **Speaker names.** Today an automatic attendee name and a user rename are stored the same way (`meeting/core.cpp` `name_speakers` and `rename_speaker`). This change adds a `speaker_meta` column to `meetings` (migration 2801) holding, for each speaker, `by` (`auto` | `user`) and `at`. Speakers without an entry, written before the change, read as `unknown`. Implicit naming labels use only `user` entries. For the four existing meetings, the user names the speakers during labelling (task 7.1).

`dev/scripts/eval.ps1` defaults `out_dir` to `<repo>/dev/eval-data/`, which `dev/.gitignore` ignores. The client accepts any absolute path the user can write to.

Alternatives considered:
- One SQLite file. Rejected: it is harder to diff and to read in review, and JSONL streams well.
- Parquet. Rejected: it needs a library, and the data is small.

### D3. Named instructions and settings
Every model instruction and tunable setting of the five steps moves out of inline string literals into one registry. Each entry has a name, a default and, for settings, an allowed range. Live code reads the defaults. A replay overlays a variant on them.

Initial names:
- triage: `triage.instructions`, `triage.batch_size`, `triage.body_limit`
- Agenda: `agenda.instructions`, `agenda.merge_instructions`. `agenda.min_confidence` stays a policy key; a replay reads it from policy and does not override it.
- knowledge: `knowledge.extraction` (the extraction instructions, including the common-sense rubric, since the two are always sent together), `knowledge.score`, `knowledge.resolve`
- diarization: `diarize.threshold`, `diarize.clusters`, which is `auto` or `attendees`. With `attendees`, the remote track is clustered into exactly as many speakers as there are attendees other than the user, when that number is at least 2. Otherwise it falls back to `auto`. The speech engine takes a fixed cluster count or a threshold, not an upper bound.
- transcription: `asr.hotword_score`, `asr.beam_paths`, `asr.vad_threshold`

The instruction id is the first 12 hex characters of the SHA-256 of the canonical JSON of all a step's entries. Triage results and Agenda items written after this change store that id (migration 2802). Facts do not: the knowledge id in effect is named in the export manifest and on each logged decision.

`diarize.clusters` and its value `attendees` exist so the speaker-naming follow-up can be measured. The live default stays `auto`, so behaviour does not change.

Alternatives considered:
- Prompt files on disk that live code reads. Rejected: it adds a tampering surface and install-time file handling.
- Overriding only through a rebuild. Rejected: it is too slow to iterate, and that was the reason for option B.

### D4. Replay isolation
The replay runs the step's pure core with:
- in-memory sinks, not the stores;
- its own call budget, `max_items`;
- notifications disabled.

Model calls go through the normal gateway client with the same Model role. Replay calls are kept out of the client's own budget tables (triage and knowledge `budget`). They are not marked for the gateway: the gateway's per-user quota and telemetry count them like any other call, which is acceptable for a developer feature and needs no control-plane change.

Alternative considered: a purpose header that the gateway would treat separately. Rejected for now, because it needs a control-plane delta for no measurement benefit. Diarization and transcription run in the speech worker at background priority, and only while no capture or live transcription is running. Results go to `runs/<run id>/results.jsonl` and `runs/<run id>/run.json`, which holds the step, the variant, the instruction id, timings, failures and token use. They are written inside the export folder.

Alternatives considered:
- Replaying against a copy of the database. Rejected: it doubles 670 MB on disk, and every store's side effects would still need disabling.

### D5. Labels
`labels.jsonl` sits beside the export and holds one label per item and step. Every label records who set it: `source` is `user_decision`, `drafted` or `user`.

- **Triage** label: `relevant` (bool), `action` (`required` | `info` | `none`), `category`, `urgency` (`now` | `this_week` | `later` | `stale`), and `why`.
- **Agenda** label: a list of commitments, each with a quote, an owner and a due date.
- **Knowledge** label: for each fact, `keep` (`important` | `ok` | `trivial` | `wrong`), and the duplicates among entities.
- **Meeting** labels:
  - a mapping from cluster to a person, an attendee email or `unknown`;
  - for 3 to 5 minutes of each meeting, a corrected reference transcript with time bounds, and speaker turns for diarization error.

How labels are made:
1. Implicit labels come from the user's own decisions:
   - Dismiss and No action needed mean action is not `required`.
   - Start work means action is `required`.
   - Agenda done means a correct item; Agenda dismissed means a false one.
   - A speaker rename gives the cluster-to-person mapping.
   - Fact deletions and edits label those facts.
2. The developer's coding assistant drafts labels for a stratified sample, with a one-line reason each. The sample is about 150 mails and 100 Teams messages, covering every category, old and new items, direct and broadcast items, and items both with and without a user decision.
3. The user reviews only the drafted labels that disagree with Skowork, or that the drafter marked unsure. They make the meeting references by correcting short transcript excerpts.

Alternatives considered:
- The user labels everything. Rejected: it is too slow.
- Implicit labels only. Rejected: they are biased towards items the user happened to touch.

### D6. Scoring tool
`dev/eval/`, written in Node.js and TypeScript per the dev-tools decision, reads an export, its labels and one or more runs, and writes `report.md`.

| Step | Metrics |
|---|---|
| Triage | precision and recall of action `required`; accuracy of `relevant`; category accuracy; urgency confusion, including the share of `stale` items marked as needing action; batch failure rate by kind; tokens per item |
| Agenda | precision and recall of commitments, using the same matching as the existing agenda corpus test |
| Knowledge | share of facts labelled `trivial` or `wrong`; share of `important` facts kept; duplicate entity pairs per 100 entities |
| Diarization and naming | diarization error rate on the labelled minutes; naming accuracy, meaning the share of speech time attributed to the right person; share of speech time left as "Speaker N" |
| Transcription | word error rate on the reference minutes; vocabulary term error rate, meaning attendee names and terms misspelled or missed |

Running two runs side by side gives deltas and lists the items whose verdict changed.

The triage batch failure rate comes from replays only, using `run.json`. Past failures are not stored per item and are not reconstructed from logs.

### D7. Baseline on this instance
The baseline covers the last 90 days of mail and Teams, the calendar for the same window, and all four meetings with audio. It runs every step with the live defaults (variant `{}`) and records the numbers below as the reference for the follow-up changes.

Baseline: not yet measured.

### D8. User decision log
The log is one append-only table, `decision_log`, in `app.db`:

| Column | Meaning |
|---|---|
| `id` | entry id |
| `at` | time of the decision |
| `step` | triage, agenda, knowledge or meeting |
| `kind` | for example dismiss, no_action, start, agenda_done, agenda_dismiss, agenda_edit, fact_edit, fact_delete, importance, speaker_rename, term_add, transcribe_again |
| `item_ref` | the Source item |
| `result_ref` | the triage row, Agenda item, fact or meeting speaker |
| `before_json`, `after_json` | the value before and after, at most 2 KB each |
| `instruction_id` | the instructions and settings that produced the result |

Each existing decision handler writes one entry. For decisions stored in `app.db` (triage, Agenda, meetings), the entry is written in the same transaction as the decision. Fact decisions are stored in `kg.db`, so they are logged right after the `kg.db` commit; a crash between the two loses only that log entry. The forget paths for items and meetings (ForgetRegistry, meeting forget) delete the matching entries. Entries older than 365 days are pruned once a day from `App::tick`, because the client has no daily housekeeping job. A heavy user is expected to write well under 100 entries a day, so the table stays below about 2 MB a year.

Why it is kept for every user and not only when evaluation is on: per-user learning will need history, and a log started only on demand begins empty.

Alternatives considered:
- Adding timestamp and history columns to each feature's own table. Rejected: four schemas would change, and history would still be lost wherever a row is overwritten (triage re-evaluation).
- Logging decisions to telemetry. Rejected: that carries content, and it is not local.

### D9. Quality counters
The client's telemetry has only an OTLP logs encoder, and its events and attributes are generated from `telemetry/catalogue.yaml`. Counters are therefore emitted as one catalogue event, `quality.daily`, per counter per day. Its attributes are:
- `quality.step`;
- `quality.counter`, a fixed enumeration;
- `quality.value`;
- `quality.instruction_id`;
- `quality.category` and `quality.action`, from their fixed enumerations (`none` outside triage).

The client version is already part of every event. Counts accumulate in a small `app.db` table, `quality_counts(day, step, counter, category, action, instruction_id, value)`, so a restart loses nothing. `App::tick` sends the previous days' rows and deletes them once the telemetry buffer has accepted them.

The "untouched after 7 days" and "still unnamed after 7 days" counts are computed in the same daily tick from local state. The table `quality_marks(kind, ref)` makes sure each item or speaker is counted only once.

Alternative considered: adding an OTLP metrics encoder. Rejected: it is new encoder and ingest work for counts that a daily log event carries just as well.

Counters do not need the evaluation setting. They follow policy.telemetry.level, like other metadata.

Alternatives considered:
- Computing the rates on the server from raw events. Rejected: the server would need per-item events, and those are closer to content.
- Making the counters part of client/evaluation. Rejected: what may leave the device is owned by client/telemetry.

## Risks / Trade-offs

- [Risk] The decision log holds before and after values that can contain personal text (an edited fact, a speaker's name). -> Mitigation: it stays in encrypted local storage, follows forget and delete, is pruned after 365 days, and leaves the device only through an evaluation export.
- [Risk] Quality counters reveal something through their attributes. -> Mitigation: the attributes are limited to fixed enumerations, the client version and the instruction id. A unit test checks the serialized metrics against an allow-list of attribute keys and values.

- [Risk] Plaintext personal data, including colleagues' mail and voices, sits in a folder synced to OneDrive. -> Mitigations:
  - the feature is off by default and reachable only on the local pipe;
  - the default folder is git-ignored;
  - `eval.ps1 -Clean` deletes an export;
  - the manifest lists what was written.

  The user accepted this on 2026-11-26 for their own data. Organizational approval for wider use stays an organizational matter, per the project constraint.
- [Risk] A developer turns the setting on in a shared or production policy. -> Mitigation: the key is owned by client/evaluation with default off, and Settings shows "Evaluation export is on" whenever it is on, so the user can see it.
- [Risk] Replay model calls cost gateway quota. -> Mitigation: `max_items` (default 200, maximum 2000), and `run.json` reports token use.
- [Risk] The refactor in D3 changes live prompts by accident. -> Mitigation: before the move, golden tests capture each instruction text exactly. After it, the same tests must pass byte for byte.
- [Risk] Exporting audio on a laptop with little free space. -> Mitigation: audio is only exported on request, and the call reports the expected size before writing. The four meetings need about 370 MB as WAV.
- [Risk] Replaying transcription competes with a live capture for CPU. -> Mitigation: it runs at background priority and pauses while capture or live transcription runs. Measuring this is in spike 1.3.
- [Risk] Older triage, Agenda and fact rows do not record which instructions produced them. -> Mitigation: they are marked `"unknown"`, and the baseline uses fresh replays, not stored results.
- [Risk] Drafted labels carry the drafter's bias. -> Mitigation: every drafted label is marked as drafted, the user reviews disagreements, and the report gives metrics separately for user-confirmed labels.

## Open Questions

- Should the export include Inbox Threads that triage prepared (the prepared draft), to evaluate preparation quality later? Proposed: not in this change.
