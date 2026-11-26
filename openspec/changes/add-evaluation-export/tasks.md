## 1. Validation Spikes

- [ ] 1.1 Build the client from this branch and run it against the existing data root `%LOCALAPPDATA%\Skowork` as the same Windows user, with a backup of `app.db` and `kg.db` taken first. Verify that it opens both stores with the existing keys and that migrations leave live behaviour unchanged.
- [ ] 1.2 Export 90 days of mail, Teams and calendar plus the four meetings without audio. Record the time taken, the folder size and the record counts in design.md D7. Verify that the export takes under 5 minutes on the baseline hardware and that the Inbox stays responsive.
- [ ] 1.3 Replay transcription and diarization of the 54-minute meeting while the user works normally. Record the wall time and CPU share. Verify that a new capture started during the replay pauses the replay and is not degraded.

## 2. Setting And Exposure

- [ ] 2.1 Add policy.evaluation.export (off | on, default off) and the `evaluation` top-level key (the glossary in openspec/config.yaml already lists it). Verify with policy parsing unit tests that a missing key or an unknown value means off.
- [ ] 2.2 Add the pipe-only exposure value to the RPC registry, and register `eval.export`, `eval.run`, `eval.status` and `eval.cancel` with it. Verify with unit tests that each method returns not_enabled and writes nothing while the setting is off, that no eval method appears in any Run's tool catalog, and that ui exposure rejects them.
- [ ] 2.3 Show "Evaluation export is on" in Settings while the setting is on. Verify with a shell test and the UI Automation name.

## 3. Named Instructions And Settings

- [ ] 3.1 Before moving anything, add golden tests that capture the exact instruction text and settings of triage, Agenda detection, knowledge extraction, scoring and resolution, diarization and transcription.
- [ ] 3.2 Move them into the registry (design D3), with ranges for the settings and the instruction id. Verify that the golden tests from 3.1 pass byte for byte, and that the registry rejects unknown names and out-of-range values.
- [ ] 3.3 Store the instruction id on triage results, Agenda items and facts written from now on. Verify with storage tests, including a migration test that existing rows read as unknown.

## 3b. Decision Log And Quality Counters

- [ ] 3b.1 Add `decision_log` (design D8) and write an entry from every decision handler, in the same transaction as the decision. Verify with unit tests for each decision kind, including that a later decision on the same item keeps the earlier entry, and that the entry carries the instruction id.
- [ ] 3b.2 Remove log entries on item or meeting forget and delete, and prune entries older than 365 days. Verify with storage tests.
- [ ] 3b.3 Emit the daily quality counters (design D9) through telemetry at the metadata and content levels, and none at off. Verify with telemetry tests, including an allow-list test on attribute keys and values, and that the 7-day counts are emitted once for each item or speaker.

## 4. Export

- [ ] 4.1 Export mail, Teams chat messages and calendar events for a window and a set of Sources (design D2), with recipients, the "user replied or sent" flag, the Skowork results and the user's decisions. Verify with tests on the Outlook and Teams fakes, the derived user_replied flag, sent mail as sent_by_user, and facts with their supporting relations (design D2), including that withheld mail is only counted, and that the stores are byte-identical before and after an export.
- [ ] 4.2a Record `named_by` (auto or user) and `named_at` on each speaker name (design D2). Verify with meeting tests that automatic naming writes auto, that a rename writes user with its time, and that older entries read as unknown.
- [ ] 4.2 Export meetings with segments, speakers, renames, attendees, the linked event, vocabulary and language. Write WAV tracks only on request, and note when audio is unavailable. Verify with tests on synthetic captures, including that a WAV round-trips to the same samples as the decoded capture.
- [ ] 4.3 Write the manifest. Report the expected size before writing audio. Verify with unit tests.
- [ ] 4.4 Fuzz the record writer with hostile item text (control characters, invalid UTF-16 surrogates, very long bodies). Verify that every line is valid JSON.

## 5. Replay

- [ ] 5.1 Replay triage, Agenda detection and knowledge extraction from an export with in-memory sinks, their own budget and the evaluation purpose header (design D4). Verify with tests on the Graph mock and the real gateway, run locally, that no store, Inbox entry, Agenda item, notification or live budget changes.
- [ ] 5.2 Replay diarization with naming, and transcription, in the speech worker at background priority, pausing during capture. Verify with tests on synthetic two-speaker audio.
- [ ] 5.3 Apply variants: reject unknown names and out-of-range values before the first item, and keep live features on the defaults. Verify with unit tests, including live triage of a new item while a variant replay is running.
- [ ] 5.4 Enforce max_items, progress, cancel and one replay at a time. Verify with unit tests.

## 6. Developer Tooling

- [ ] 6.1 Add `dev/scripts/eval.ps1` (export, run, status, cancel, -Clean) and add `eval-data/` to `dev/.gitignore`. Verify by running it against a dev build with the Graph mock.
- [ ] 6.2 Add `dev/eval`, written in Node.js and TypeScript: the label schema (design D5), import of implicit labels from user decisions, the scoring and comparison report (design D6) and a sampler for the labelling set. Verify with unit tests on a synthetic export committed under `dev/eval/fixtures`, which contains no real data.

## 7. Baseline On This Instance

- [ ] 7.1 Export 90 days and the four meetings with audio. Import the implicit labels. Draft labels for the stratified sample. Have the user review the disagreements and unsure items, and correct 3 to 5 minutes per meeting and name the speakers.
- [ ] 7.2 Replay every step with the live defaults. Record the baseline metrics in design.md D7 and list the 10 worst triage items and the meeting naming errors as input to the follow-up changes. Verify that the report is reproducible: a second run with the same variant differs only by model nondeterminism, within ±3 points on each rate.

## 8. Close

- [ ] 8.1 Run `openspec validate --all --strict`, then archive, then reread openspec/specs/client/evaluation/spec.md for coherence.
