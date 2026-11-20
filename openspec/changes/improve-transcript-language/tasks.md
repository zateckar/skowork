## 1. Validation spikes

- [ ] 1.1 Needs the user's OK because it records and keeps real meeting audio: with policy.meeting_capture.keep_audio on for the test Group, record two Czech meetings of at least 10 minutes on the test machine, one with loudspeakers and one with a headset. Verify: the audio is kept, and the number of segments in another script, short fragments in another language, echo copies and "(unclear)" candidates per 100 segments is recorded in design.md Acceptance as the baseline.
- [ ] 1.2 Decode the recordings of 1.1 with the measures of D1–D3 in a development build. Verify: per 100 segments, other-script segments are 0, short foreign fragments drop by at least two thirds, no genuine user speech is removed as echo, and the transcription time stays within the Transcription Resource Budget; all numbers are recorded in design.md Acceptance.

## 2. Decoding in context

- [x] 2.1 Write tests: spans of one track with gaps under 1 s form one unit of at most 20 s; a span longer than 20 s stays on its own; text splits back onto the spans by token time; an isolated span is decoded alone. Verify: the tests fail before 2.2 and pass after it. (2026-11-20: `MeetingLanguage.DecodingUnitsJoinShortPausesAndSplitByTime`; written alongside 2.2.)
- [x] 2.2 Join spans into decoding units in the sherpa-onnx engine and split the text back by token timestamps. Verify: the tests of 2.1, the sherpa fixture tests and the memory trace of the 60-minute meeting stay within the 2 GB Job limit. (2026-11-20: the sherpa fixture tests pass except the Czech one, whose fixture is missing on the test machine; the 60-minute meeting peaked at 1,452 MB private, see show-meeting-transcription-progress design Acceptance.)

## 3. The language check

- [ ] 3.1 Write tests: Cyrillic text in a Czech meeting fails; a two-word English fragment in a Czech meeting fails; a ten-word English sentence passes; "OK" passes; a failed segment decoded again in Czech replaces the text; a second failure and a known Whisper hallucination become "(unclear)"; "(unclear)" segments are not in the outputs message or the knowledge turns. Verify: the tests fail before 3.2 and pass after it, and the segment parser's fuzz target runs. (2026-11-20: the check, the hallucination list and "(unclear)" in outputs, knowledge and storage are covered by `MeetingLanguage.OtherScriptAndForeignFragments` and `MeetingLanguage.UnclearSegmentsStayOutOfOutputsAndKnowledge`. Open: decoding again in the engine has no automated test, because no committed fixture makes Parakeet write a foreign fragment; spike 1.2 covers it on real audio. The fuzz target was not run.)
- [x] 3.2 Add the function-word lists for cs, de and en, the check, and decoding again with Whisper small. Verify: the tests of 3.1 pass. (2026-11-20: without context padding, see design D2 As built.)

## 4. Echo near copies

- [x] 4.1 Write a test from the pattern of 2026-11-20: a local segment that garbles a remote segment at the same time, with audio evidence, is echo; a read-back two seconds later is not. Verify: it fails before 4.2 and passes after it, and the existing echo tests still pass. (2026-11-20: `MeetingEcho.GarbledCopyIsEchoAReadBackIsNot`, with invented sentences in the same pattern, not the user's meeting; written alongside 4.2.)
- [x] 4.2 Add the character-similarity match to the echo rule. Verify: the tests of 4.1 pass.

## 5. Language choice and transcribing again

- [x] 5.1 Add the language rows to the Record meeting list and `meetings.set_language`; add "Transcribe again in…" to the meeting Thread while its audio is kept, with strings in cs, de and en. Verify: a service test transcribes with the chosen language without detection and replaces the transcript on transcribing again; a shell test covers the rows and the action. (2026-11-20: `MeetingService.LanguageChoiceAndTranscribeAgain`, `ShellMeetings.RecordMeetingFromANewThread`, `ShellMeetings.ProgressLineInTheMeetingThread`.)

## 6. Verification

- [ ] 6.1 Build and run the client tests: `cmake --build --preset test` in `client/`, then `win_tests.exe` and `ui_tests.exe`. Verify: all pass, and the results are recorded in design.md Acceptance.
- [ ] 6.2 After add-meeting-capture and improve-meeting-capture-quality are archived and every task above is checked, run `openspec archive improve-transcript-language --yes`, then reread the merged `openspec/specs/client/meeting-capture/spec.md`. Verify: the archive succeeds and the merged spec reads consistently.
