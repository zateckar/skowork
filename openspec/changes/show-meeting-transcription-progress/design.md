# Design

## Context

The motivation is in proposal.md. The pipeline is add-meeting-capture design D4: Silero VAD per track, diarization of the remote track, language identification, Parakeet in batches, merge by time. The stt worker answers `stt.transcribe` once, with the whole result; it reports no progress. The live pass (`MeetingService::live_step`) sends every track file in full every 60 seconds, and the final pass (`transcribe_one`) sends them in full again.

Evidence from the test machine on 2026-11-20 (release build `0.1.1-dev.29.manualcapture`, policy live_transcript on): live passes started 60 seconds after the previous one ended, and their spacing grew from about 1.5 to more than 2.5 minutes over a 19-minute meeting; the final pass took 2 minutes 4 seconds after the first meeting and 2 minutes 44 seconds after the second.

## Decisions

### D1. Live passes transcribe only new speech
- **Decision:** `stt.transcribe` takes `from_ms` and `diarize` (default true). The worker runs VAD over the whole track, so spans are the same in every pass, and decodes only spans that start at or after `from_ms`. It reports `audio_ms`, the audio every track has on disk: a track's writer seals about one second at a time, so the newest second may not be readable yet. The service cuts the pass 2 seconds before `audio_ms`; segments that end after the cut, and everything from the first of them, are decoded again by the next pass. It keeps the rest with the segments of earlier passes and stores the next `from_ms` with the meeting (`live_from_ms`, migration 2100).
- **Speakers:** live passes send `diarize: false` and get one provisional speaker per track, which is not shown. The final pass after the capture decodes from `live_from_ms`, diarizes each remote track whole once, and returns the turns (`turns: {track: [[start, end, cluster]]}`); the service labels every segment, kept or new, with `label_spans`, as D4 step 2 does within one request. A separate `stt.diarize` request was not needed.
- **Language:** the first pass with enough speech identifies the language as today; later passes reuse it (the service sends `language`).
- **As built (2026-11-20):** each pass still reads and decrypts the whole track files and runs Silero VAD over them, which grows with the meeting but is small next to decoding (spike 1.1).
- **Alternatives considered:**
  - Keep full passes and show progress only. Rejected: the work grows with the square of the meeting's length, and the user waits for a full second transcription after the meeting.
  - Diarize incrementally with persistent embeddings. Rejected for now: sherpa-onnx's offline diarization clusters a whole recording; incremental clustering would be our own code and a new risk. One diarization at the end costs a fraction of ASR (D4 measured 654 MB peak, time to be recorded in spike 1.1).
  - Stream audio to a resident worker. Rejected: the worker's idle exit and memory budget (client/desktop-host) assume short requests.

### D2. Progress comes from the service, not the worker
- **Decision:** the service knows the meeting time a request covers before it sends it: the meeting's length (`ended_at - started_at`) minus `live_from_ms`. It records, per pass of at least 10 seconds of audio, meeting milliseconds and wall-clock milliseconds, keeps a moving speed (60% previous, 40% new) in `meeting_state` (migration 2100), and estimates the share done during a request as elapsed time times speed over the meeting time to transcribe, capped at 95% until the answer arrives. Before a speed is measured it assumes 4 meeting seconds per second for the share and shows no time left.
- **Why audio, not speech:** knowing the speech length needs VAD over the tracks before the request, a second pass over the audio for an estimate. Audio length is free, and the measured speed already folds in the share of speech typical of this user's meetings.
- **Alternatives considered:**
  - Progress messages from the worker during a request. Rejected for now: the worker channel is request and response only (client/desktop-host workers), and adding notifications touches every worker role.
  - A spinner without numbers. Rejected: the user asked to know how long it takes.

### D3. The progress line belongs to the meeting Thread
- **Decision:** a new request `meetings.progress {id}` returns `stage` (asking, recording, waiting, transcribing, summarizing, done, failed), `reason`, `elapsed_ms`, `done` (0–1), `left_ms`, `error`, `can_transcribe_now` and `can_transcribe_again`; `meetings.status` carries the same per meeting as `progress`. The Thread view shows the line under the Thread's origin for Threads whose origin is a meeting, loads it again on `meetings.changed`, and polls every 5 seconds while the stage is not done or failed. The line's text is announced to screen readers on a change of stage only. A done meeting shows no line unless it can be transcribed again.
- **Why a request of its own:** `meetings.status` carries every meeting's transcript; polling it every 5 seconds would move the transcripts of up to 50 meetings each time.
- **Alternatives considered:** a message in the feed for each stage. Rejected: it would leave stale messages in the Thread and in its knowledge item.

## Risks / Trade-offs

- [A sentence cut at a pass boundary is decoded twice or split] -> The 2-second margin re-decodes the last segment whole; tests cover a segment that spans a pass boundary.
- [Speakers differ between live passes and the final diarization] -> Provisional speakers are never shown; only the final assignment is stored with the transcript.
- [The estimate is wrong on a busy computer] -> The share is capped at 95% until the answer arrives, and the speed is a moving average of recent passes.
- [The final diarization of a long meeting still takes time] -> Spike 1.1 measures it for 20 and 60 minutes on the test machine and the baseline laptop; the progress line covers it as its own step.

## Acceptance

Results are recorded here as tasks complete.

- **1.1 (2026-11-20, test machine, 8 threads, test build with the release sherpa-onnx DLLs, `SKOWORK_STT_TRACE`):** `MeetingSherpa.DISABLED_SixtyMinuteMeeting`, 60 minutes on two tracks, with this change and improve-transcript-language's decoding units: 252 s in all (real-time factor 0.070; the 2026-10-27 measurement in add-meeting-capture D4 was 548 s on another machine). Reading the tracks and VAD about 28 s, diarization of the remote track 65 s (26%, peak 624 MB), Parakeet 159 s (peak 1,452 MB, inside the 2 GB Job limit). The final pass after live transcription is reading, VAD, diarization and the last minute: about 1.5 minutes for an hour, about 30 s for 20 minutes, on this machine. The baseline laptop has a quarter of the threads; task 4.2 checks it there.
- **4.1 (2026-11-20, test machine, `client/build/test-local`; the copied `client/build/test` cache points at a toolchain path that does not exist here):** `win_tests.exe` 1,010 tests: 978 passed, 30 skipped, 2 failed. `MeetingSherpa.VocabularyHotwordsAndRespelling` fails because its Czech fixture `parakeet-cs.wav` is not on this machine (it failed the same way before these changes). `ShellFeed.CtrlCWithTheComposerFocusedCopiesTheMessageSelection` reads the system clipboard right after Ctrl+C and fails intermittently on this desktop, also before these changes. One earlier full run in brief mode stopped after a mail test without a summary; the mail and knowledge tests then passed alone, and a full run passed through, so the cause is unknown. `ui_tests.exe`: 104 passed.
