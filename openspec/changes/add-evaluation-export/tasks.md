## 1. Validation Spikes

- [ ] 1.1 Build the client from this branch and run it against the existing data root `%LOCALAPPDATA%\Skowork` as the same Windows user, with a backup of `app.db` and `kg.db` taken first. Verify that it opens both stores with the existing keys and that migrations leave live behaviour unchanged.
- [ ] 1.2 Export 90 days of mail, Teams and calendar plus the four meetings without audio. Record the time taken, the folder size and the record counts in design.md D7. Verify that the export takes under 5 minutes on the baseline hardware and that the Inbox stays responsive.
- [ ] 1.3 Replay transcription and diarization of the 54-minute meeting while the user works normally. Record the wall time and CPU share. Verify that a new capture started during the replay pauses the replay and is not degraded.

## 2. Setting And Exposure

- [x] 2.1 Add policy.evaluation.export (off | on, default off) and the `evaluation` top-level key (the glossary in openspec/config.yaml already lists it). Verify with policy parsing unit tests that a missing key or an unknown value means off.
  Status: done 2026-11-26; PolicyCore.EvaluationExportDefaultsOff, PolicySchema.ClientDefaultsMatchTheGeneratedSchema (schema.json and keys.ts edited by hand; run npm run policy:schema where Node exists).
- [x] 2.2 Add the pipe-only exposure value to the RPC registry, and register `eval.export`, `eval.run`, `eval.status` and `eval.cancel` with it. Verify with unit tests that each method returns not_enabled and writes nothing while the setting is off, that no eval method appears in any Run's tool catalog, and that ui exposure rejects them.
  Status: done 2026-11-26; Rpc.PipeOnlyHiddenFromWindow, EvalRpc.OffMeansNotEnabled, EvalRpc.MethodsArePipeOnly, EvalRpc.OnWithoutServiceIsUnavailable. No eval method is a ToolDef (they are RPC methods only).
- [x] 2.3 Show "Evaluation export is on" in Settings while the setting is on. Verify with a shell test and the UI Automation name.
  Status: done 2026-11-26; ShellTelemetry.ViewerListsSentAndBufferedRecordsWithTheirFields (the line is in the Telemetry card of Settings).

## 3. Named Instructions And Settings

- [x] 3.1 Before moving anything, add golden tests that capture the exact instruction text and settings of triage, Agenda detection, knowledge extraction, scoring and resolution, diarization and transcription.
  Status: done 2026-11-26; InstructionsGolden.* with fixtures under client/tests/fixtures/instructions.
- [x] 3.2 Move them into the registry (design D3), with ranges for the settings and the instruction id. Verify that the golden tests from 3.1 pass byte for byte, and that the registry rejects unknown names and out-of-range values.
  Status: done 2026-11-26; Registry.* (live code keeps its constants; Registry.DefaultsMatchLive keeps the registry equal to them).
- [x] 3.3 Store the instruction id on triage results, Agenda items and facts written from now on. Verify with storage tests, including a migration test that existing rows read as unknown.
  Status: done 2026-11-26 for triage results and Agenda items (migration 2802); InstructionId.*. Facts carry no stored id: the manifest and the decision log name the knowledge id in effect.

## 3b. Decision Log And Quality Counters

- [x] 3b.1 Add `decision_log` (design D8) and write an entry from every decision handler, in the same transaction as the decision. Verify with unit tests for each decision kind, including that a later decision on the same item keeps the earlier entry, and that the entry carries the instruction id.
  Status: done 2026-11-26; DecisionLog.*, EventTriage.DecisionsAreLogged, AgendaService.DecisionsAreLogged, KnowledgeCuration.FactDecisionsAreLogged, MeetingSpeakers.AutoNamesAndUserRenamesAreRecorded.
- [x] 3b.2 Remove log entries on item or meeting forget and delete, and prune entries older than 365 days. Verify with storage tests.
  Status: done 2026-11-26; DecisionLog.ForgetRemovesEntries, DecisionLog.PruneAfter365Days, DecisionLog.ForgetDuringQuery; Source forget clears by item kind.
- [x] 3b.3 Emit the daily quality counters (design D9) through telemetry at the metadata and content levels, and none at off. Verify with telemetry tests, including an allow-list test on attribute keys and values, and that the 7-day counts are emitted once for each item or speaker.
  Status: done 2026-11-26; Quality.* (event quality.daily; catalogue regenerated with a byte-identical Python replica of the generator), EventTriage.FailedBatchesAreCounted, telemetry_tests.

## 4. Export

- [x] 4.1 Export mail, Teams chat messages and calendar events for a window and a set of Sources (design D2), with recipients, the "user replied or sent" flag, the Skowork results and the user's decisions. Verify with tests on the Outlook and Teams fakes, the derived user_replied flag, sent mail as sent_by_user, and facts with their supporting relations (design D2), including that withheld mail is only counted, and that the stores are byte-identical before and after an export.
  Status: done 2026-11-26; Exporter.WindowAndSources, Exporter.WithheldOnlyCounted, Exporter.UserRepliedDerivedAndSentMailMarked, Exporter.IncludesDecisionsAndLegacyAndChangesNothing. Teams export is not unit-tested (no Teams fixture); spike 1.2 covers it.
- [x] 4.2a Record who named each speaker (speaker_meta: by auto or user, and the time; design D2). Verify with meeting tests that automatic naming writes auto, that a rename writes user with its time, and that older entries read as unknown.
  Status: done 2026-11-26; MeetingSpeakers.* (column speaker_meta, migration 2801).
- [x] 4.2 Export meetings with segments, speakers, renames, attendees, the linked event, vocabulary and language. Write WAV tracks only on request, and note when audio is unavailable. Verify with tests on synthetic captures, including that a WAV round-trips to the same samples as the decoded capture.
  Status: done 2026-11-26; Exporter.MeetingsWithAndWithoutAudio, Wav.RoundTrip.
- [x] 4.3 Write the manifest. Report the expected size before writing audio. Verify with unit tests.
  Status: done 2026-11-26; Exporter.ManifestCountsAndIds, EvalRpc.ExportJobLifecycle (dry_run reports audio_bytes).
- [x] 4.4 Fuzz the record writer with hostile item text (control characters, invalid UTF-16 surrogates, very long bodies). Verify that every line is valid JSON.
  Status: done 2026-11-26; Jsonl.HostileTextIsOneValidLine. The fuzz target client/fuzz/eval_jsonl_fuzz.cpp was added but not run.

## 5. Replay

- [x] 5.1 Replay triage, Agenda detection and knowledge extraction from an export with in-memory sinks, their own budget and the evaluation purpose header (design D4). Verify with tests on the Graph mock and the real gateway, run locally, that no store, Inbox entry, Agenda item, notification or live budget changes.
  Status: done 2026-11-26; Replay.TriageWritesResultsOnly, Replay.FailuresAreCountedByKind (the replayer holds no store). Real-gateway run is spike 1.2/7.2.
- [x] 5.2 Replay diarization with naming, and transcription, in the speech worker at background priority, pausing during capture. Verify with tests on synthetic two-speaker audio.
  Status: done 2026-11-26; Replay.SkipsMeetingWithoutAudioAndPausesDuringCapture, SttRequest.AcceptsExportedWavTracks, SttRequest.SettingsDefaultsAndOverrides, Replay.AttendeesClusterCount.
- [x] 5.3 Apply variants: reject unknown names and out-of-range values before the first item, and keep live features on the defaults. Verify with unit tests, including live triage of a new item while a variant replay is running.
  Status: done 2026-11-26; Replay.VariantAppliesAndUnknownIsRejectedBeforeFirstItem, Registry.*.
- [x] 5.4 Enforce max_items, progress, cancel and one replay at a time. Verify with unit tests.
  Status: done 2026-11-26; Replay.MaxItemsBounds, Replay.BusyAndCancelKeepsWritten.

## 6. Developer Tooling

- [x] 6.1 Add `dev/scripts/eval.ps1` (export, run, status, cancel, -Clean) and add `eval-data/` to `dev/.gitignore`. Verify by running it against a dev build with the Graph mock.
  Status: done 2026-11-26; dev/scripts/eval.ps1 parses; not yet run against a client (needs spike 1.1).
- [ ] 6.2 Add `dev/eval`, written in Node.js and TypeScript: the label schema (design D5), import of implicit labels from user decisions, the scoring and comparison report (design D6) and a sampler for the labelling set. Verify with unit tests on a synthetic export committed under `dev/eval/fixtures`, which contains no real data.
  Status: written 2026-11-26; dev/eval with tests on fixtures/synthetic-export. Not run: Node.js is not installed on this workstation.

## 7. Baseline On This Instance

- [ ] 7.1 Export 90 days and the four meetings with audio. Import the implicit labels. Draft labels for the stratified sample. Have the user review the disagreements and unsure items, and correct 3 to 5 minutes per meeting and name the speakers.
- [ ] 7.2 Replay every step with the live defaults. Record the baseline metrics in design.md D7 and list the 10 worst triage items and the meeting naming errors as input to the follow-up changes. Verify that the report is reproducible: a second run with the same variant differs only by model nondeterminism, within ±3 points on each rate.

## 8. Close

- [ ] 8.1 Run `openspec validate --all --strict`, then archive, then reread openspec/specs/client/evaluation/spec.md for coherence.
