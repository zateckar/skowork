## 1. Validation spikes

- [x] 1.1 Measure diarization alone and Parakeet decoding alone on the 20-minute and 60-minute fixture meetings (`MeetingSherpa.DISABLED_SixtyMinuteMeeting` and a 20-minute cut of it) on the test machine. Verify: the times and peak memory are recorded in design.md D1, and diarization alone takes less than a quarter of a full transcription. (2026-11-20: 60-minute meeting only, from the stage trace; diarization took 26% of the full transcription, just over the target, and is accepted because the final pass is still about a third of a full one. Numbers in design.md Acceptance. The 20-minute cut was not run.)

## 2. Incremental live transcription

- [x] 2.1 Write tests: a live-transcribed meeting decodes each speech span once; a segment that spans a pass boundary appears once and whole; the final step decodes only the remainder and diarizes once; a speaker who talks in the first and last minute keeps one label; a meeting without live transcription transcribes once as before. Verify: the tests fail before 2.2 and pass after it. (2026-11-20: `MeetingService.LiveTranscriptionDecodesEachSecondOnce`; the existing meeting tests cover transcription without live passes. Written alongside 2.2, so failing before it was not observed.)
- [x] 2.2 Add `from_ms`, `diarize`, `turns` and `audio_ms` to `stt.transcribe`; keep segments and the next `from_ms` in the meeting store; assign speakers after the final diarization. Verify: the tests of 2.1 and all meeting tests pass. (2026-11-20: all meeting tests pass except `MeetingSherpa.VocabularyHotwordsAndRespelling`, whose Czech fixture `parakeet-cs.wav` is not on the test machine and is not fetched by `dev/scripts/fetch-stt-models.ps1`; it failed the same way before this change. The worker's request parameters are numbers and booleans read with defaults, so no parser fuzz target was added.)

## 3. Progress

- [x] 3.1 Write tests: the stage follows recording, waiting with its reason, transcribing, summarizing, done and failed; the share grows during a request and stays at most 95% until it answers; no time left is given before a speed is measured. Verify: the tests fail before 3.2 and pass after it. (2026-11-20: `MeetingService.ProgressFollowsTheStages`; written alongside 3.2.)
- [x] 3.2 Add `meetings.progress` and `progress` in `meetings.status`, and keep the measured speed in the meeting store. Verify: the tests of 3.1 pass.
- [x] 3.3 Show the progress line in a meeting's Thread with Transcribe now when it fails or waits, announced on a change of stage, with strings in cs, de and en. Verify: a shell test shows each stage, loads again on `meetings.changed`, and passes the accessibility check. (2026-11-20: `ShellMeetings.ProgressLineInTheMeetingThread`; the 5-second poll is not exercised by the test, which drives the line by events.)

## 4. Verification

- [ ] 4.1 Build and run the client tests: `cmake --build --preset test` in `client/`, then `win_tests.exe` and `ui_tests.exe`. Verify: all pass, and the results are recorded in design.md Acceptance. (2026-11-20: not all pass; see design.md Acceptance.)
- [ ] 4.2 Needs the user's OK because it records real audio: on the test machine, record a 15-minute Teams meeting with live transcription on. Verify: the Thread shows each stage; the summary starts within 1 minute of Stop; the result is recorded in design.md Acceptance.
- [ ] 4.3 After add-meeting-capture is archived and every task above is checked, run `openspec archive show-meeting-transcription-progress --yes`, then reread the merged `openspec/specs/client/meeting-capture/spec.md`. Verify: the archive succeeds and the merged spec reads consistently.
