# Design

## Context

The motivation is in proposal.md. The user decided that speech-to-text runs on the CPU, on the baseline hardware: Intel Core i5, 16 GB RAM, integrated GPU only. The client is C++20/Win32. Other capabilities supply the pieces this one needs:
- worker processes, from client/desktop-host and client/tool-sandbox;
- calendar data, from client/mail-calendar;
- the Trigger kind meeting, from client/scheduler, which consumes this capability's meeting events;
- Threads and the presentation of the consent Question as a standalone Action item, from client/work-inbox. The recording indicator is drawn by this capability itself, as a topmost overlay;
- background deferral and worker lifetime, from client/desktop-host;
- untrusted-content tagging, from client/agent-runtime, and its rules, from client/policy-enforcement.

## Goals / Non-Goals

**Goals:**
- High-quality Czech, German and English transcripts produced locally within the budget.
- Capture that is consent-first and visibly indicated.
- Clean separation of local and remote speech.

**Non-Goals:**
- Live captions as a default.
- A meeting bot.
- Server-side transcription.
- GPU or NPU acceleration (possible later, but not assumed).

## Decisions

### D1. Speech-to-text: NVIDIA Parakeet-TDT-0.6B-v3 at int8, run through sherpa-onnx on CPU
- **Why this model:** it is multilingual across 25 European languages, including cs, de, en, sk and pl. It has automatic punctuation and casing, and is far faster on CPU than Whisper large models. The expected real-time factor is about 0.05–0.15 on 4 threads of an i5 (to be confirmed by spike 1.1), well inside the 0.5 budget.
- **Size:** about 650 MB on disk at int8. Resident memory during decoding is about 1–1.5 GB.
- **Licence:** CC-BY-4.0, with attribution in About and the licence notices.
- **Alternatives considered:**
  - Whisper large-v3-turbo via whisper.cpp at q5: the best Czech accuracy in some benchmarks, but a real-time factor of about 0.5–1.0 on an i5 CPU, which is at or over budget.
  - Whisper small or medium: fast enough but noticeably worse for Czech. Medium also does not fit the models budget.
  - Kept as fallback: Whisper small only (q5, about 190 MB), for languages Parakeet does not cover, selected by language detection.
  - Cloud or on-prem server ASR: rejected because the spec requires local processing.
- **Models budget:** the optional models package holds only speech-to-text models, because embeddings and reranking use the on-prem models through the LLM gateway. Parakeet (about 650 MB), Whisper small (about 190 MB), and VAD and diarization (about 40 MB) total about 0.88 GB. That is within the models budget of client/desktop-host (≤ 1.0 GB at first, 1.2 GB since 2026-10-31; see As built).
  - **As built (2026-10-27):** sherpa-onnx runs Whisper as ONNX, and its int8 Whisper small is 357 MB (encoder 107 MB, decoder 250 MB), not the 190 MB of a whisper.cpp q5 file. With Parakeet int8 (640 MB), pyannote segmentation int8 (1.5 MB), WeSpeaker ResNet34-LM (25 MB), Silero VAD (0.6 MB) and the runtime DLLs (21 MB), the package is about 1,046 MB (1.02 GB), just over the 1.0 GB budget. Options: accept it by amending the budget in client/desktop-host, drop Whisper small (no supported language needs it for ASR, and Parakeet cannot identify languages, so the language would then come from the calendar or the user's locale), or use Whisper base (about 160 MB as int8 ONNX, weaker language identification on short speech). Decided 2026-10-31: the budget in client/desktop-host is raised to 1.2 GB (add-client-foundation), and the package stays as built.

### D2. Capture: WASAPI process loopback plus the microphone
- **Remote track:** `ActivateAudioInterfaceAsync` with `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK` and `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE`, targeting the meeting app's process tree (Windows 10 2004 or later).
- **Local track:** the default communications microphone, captured in shared mode.
- **Format and storage:** both tracks are 16 kHz mono, encrypted with a per-capture key wrapped by DPAPI.
- **Implementation (2026-10-26):** no Opus or FLAC encoder is vendored, so tracks hold 16-bit PCM (about 115 MB per hour and track) in a small container: a header (magic `SKAU`, version, track kind, sample rate, start time, the DPAPI-wrapped AES-256 key) and chunks of about one second, each sealed with AES-256-GCM under its own nonce, with the chunk's sample offset as associated data. WASAPI converts to 16 kHz mono itself (`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`). A FLAC encoder can replace the PCM payload later without changing the container.
- **Why:** process loopback excludes other system sounds. Separate tracks give "me vs. others" for free and improve diarization.
- **Alternative considered:** system-wide loopback. Kept for manual "all system output" capture only.

### D3. Detection
- **Audio sessions:** `IAudioSessionManager2` notifications give the audio sessions and their process ids.
- **Microphone use:** the CapabilityAccessManager registry state, or an active capture endpoint session.
- **Meeting sites in browsers:** checked only by the top-level window title, read through UI Automation, against the patterns in policy.meeting_capture.apps. CDP is not used: the agent's own browser profile is not where users join meetings, and attaching to the user's browser is out of scope.
- **Calendar match:** events within ±10 minutes that have an online-meeting URL.
- **Events:** "meeting detected" on detection and "transcript ready" after outputs are complete. Both go to the in-process event bus for client/scheduler (Trigger kind meeting).
- **Implementation (2026-10-26):** the detector polls every 2 seconds while capture is enabled. It takes the active sessions of every render endpoint, maps each process to its ancestors (Toolhelp snapshot) to find the app entry, and reads microphone use from the CapabilityAccessManager consent store (`LastUsedTimeStop` of 0), read only. Window titles of browser entries are read through UI Automation (`ElementFromHandle`, `CurrentName`). A meeting ends after 30 seconds without an active session of its app. The calendar match takes the closest event within 10 minutes that has an online-meeting link; its series id (seriesMasterId) is the key for series auto-capture.
- **Trigger source:** while capture is enabled, the meeting kind is registered with client/scheduler; its conditions are in the spec (Meeting Trigger Conditions), and a firing's detail carries ids only (`meeting_event`, `meeting_id`, `thread_id`, `calendar_event_id`), which the scheduler appends to the Trigger's prompt. The key is `meeting_event`, not `event`, because the scheduler keeps details with an `event` key (session Triggers) out of the prompt.

### D4. Pipeline in the stt worker
1. Silero VAD segments the audio.
2. Language identification runs on the first speech segments.
3. Parakeet runs ASR per segment.
4. sherpa-onnx speaker diarization (segmentation plus embedding clustering) runs on the remote track only.
5. Tracks are merged by timestamp.
6. Speakers are named where the match is confident (spec: Speaker Diarization And Naming): one remote cluster and exactly one other attendee. Everyone else is "Speaker N" until the user renames them. Matching several clusters to attendees with the user's confirmation, and voice profiles from earlier renames, are not part of this change (Open Questions).
7. Summary, decisions and follow-ups are generated by the `main` model role through client/model-access, in chunks, using a map-reduce over the transcript.

Worker settings: half the logical cores and BELOW_NORMAL priority. The Job Object memory limit (2 GB) and the idle exit come from the stt-worker budget of client/desktop-host.

- **Engine interface (2026-10-26):** the pipeline runs against a `SpeechEngine` interface. Development builds also carry a test engine that turns each speech segment into a numbered line and splits the remote track into two speakers by loudness, so the worker, the container and the pipeline are tested end to end without models; it stays their default engine, and `SKOWORK_STT_ENGINE=sherpa-onnx` (or a request's `engine`) selects the real one. Release builds always use sherpa-onnx.
- **sherpa-onnx engine (2026-10-27, user-approved download):** sherpa-onnx 2.0.1 (MT-Release, no TTS) through its C API. `sherpa-onnx-c-api.dll` and `onnxruntime.dll` are vendored in client/third_party/sherpa-onnx and belong to the models package: they sit in `models\stt` beside the worker, which loads them with `LoadLibraryEx` on first use, so Skowork.exe links nothing new and a client without the package reports `model_missing`. The models are read from the same folder; development builds fall back to `client/models/stt`, fetched by `dev/scripts/fetch-stt-models.ps1` with the hashes in client/third_party/README.md, and `SKOWORK_STT_MODELS` overrides the folder. sherpa-onnx opens model files by narrow path, so a models folder whose name the ANSI code page cannot hold fails with `model_path` rather than a crash; installing into a folder with an ASCII path avoids it. The steps:
  1. Silero VAD per track (threshold 0.5, 0.4 s minimum silence, 0.25 s minimum speech, segments of at most 20 s, which Whisper needs and which keeps transcript timing fine).
  2. Every track except the user's own (`local`) is diarized whole: pyannote segmentation 3.0 (int8) and WeSpeaker ResNet34-LM embeddings, clustered by a 0.5 distance threshold because the number of speakers is unknown. Each speech segment takes the cluster it overlaps most (the nearest turn in a gap), numbered `s1`, `s2`, ... by first appearance, in one sequence across tracks. A track with one speech segment is one speaker. Diarization runs before ASR so its models and Parakeet are never loaded together.
  3. Without a requested language, Whisper small identifies the language on up to three windows of 8–30 s of speech, the user's own track first, and the majority wins.
  4. Parakeet decodes the speech segments of the languages it covers, in batches of 8; a supported language outside them goes to Whisper small with that language set. All supported languages (cs, de, en, sk, pl) are Parakeet languages, so Whisper small serves language identification; development builds can force the Whisper path (`asr: "whisper"`) to test it.
  5. Segments come back with track, times, speaker and text; the service merges the tracks by time (parse_segments) and names speakers (step 6).
  Threads: half the logical cores for each model, under the Job's 50% CPU cap and 2 GB memory limit.
  - **Memory (2026-10-27):** two settings keep an hour inside the 2 GB Job limit, found by tracing the worker's private bytes per stage (development builds: `SKOWORK_STT_TRACE=<file>`). Diarization uses a 5 s step over pyannote's 10 s windows (window shift ratio 0.5) instead of the default 1 s: clustering keeps a distance matrix over every window and local speaker, about 10,800 embeddings an hour at 1 s, which ran out of memory; at 5 s it is about 2,200 and diarization of an hour peaks at 654 MB. An ASR batch holds at most 20 s of audio (up to 8 segments): onnxruntime keeps its arena at the largest batch it has seen, and a 40 s batch took the worker from 943 MB to 1,483 MB, after which a later one crossed 2 GB.
  - **Measured (2026-10-27, development workstation, i7-12700, debug build of the worker code with the release sherpa-onnx DLLs):** a 60-minute meeting on two tracks (the `MeetingSherpa.DISABLED_SixtyMinuteMeeting` test builds it from the fixture recordings: three remote voices and the user, turns with pauses) transcribed in 548 s, a real-time factor of 0.15, on 10 threads under the 50% CPU cap. Worker peak 1,482 MB private (VAD 266 MB, diarization 654 MB, Parakeet loaded 942 MB). The result had 445 segments and 4 speakers ("me" and three remote, matching the fixture). The fixture meeting test (36 s) takes about 11 s including loading the models. The baseline laptop (4 cores, so 2 threads) is expected to be 3–4 times slower, close to the 0.5 budget; task 4.1 measures it there.
- **Outputs (2026-10-26):** the meeting Thread gets one Unattended run whose message is the transcript, delivered as untrusted content with the meeting as provenance (client/agent-runtime `StartOptions.untrusted`); the preamble asks for a summary, decisions and follow-ups, which are the Run's answer. A transcript longer than about 48,000 characters is first summarised in parts with the `main` role through client/model-access (the map step), and the Run gets the partial notes with the transcript. The transcript and speakers are stored with the meeting in `app.db` (migration 1200), and passed to client/knowledge-graph as the meetings Source with each named speaker as a Person; renaming a speaker re-indexes the meeting, which re-links the mentions.

### D5. Retention and storage
- **Before transcription:** pending audio lives in the `capture/` directory under the data root, encrypted.
- **After transcription:** the transcript is stored in `app.db` (on the Thread). It is indexed by client/knowledge-graph when policy.knowledge.sources.meetings is on.
- **Deletion:** audio is deleted after a successful transcript, unless keep_audio is set. A daily sweep enforces max_pending_days.

### D6. Consent flow and indicator
- **Consent:** "Start capture?" is a standalone Action item (client/work-inbox) in the meeting-origin Thread with Start and Not now, plus "Always capture this series" when policy.meeting_capture.allow_series_auto is on and the event is part of a series. It is withdrawn after 2 minutes without an answer.
- **Notice:** with require_notice on, Start raises a second Question whose payload carries the notice text as `copy`; the Action card shows it with a Copy button, and capture starts on "I posted the notice".
- **Indicator:** a small Win32 window (`WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`) owned by the core process, not by the main window, with the elapsed time and a Stop button; Stop ends both capture threads and closes the tracks within 2 seconds.
- **Implementation (2026-10-26):**
  - An opted-in series skips the consent Question but not the notice: with require_notice on, capture starts after "I posted the notice".
  - The notice goes out before the meeting language is known, so it uses the UI language (plus English).
  - A declined, ignored or ended-before-consent meeting keeps no Thread: the meeting-origin Thread only held the Question and is deleted through the forget cascade.
  - Speakers are numbered "Speaker N" in order of first appearance. A rename updates the transcript in the Meetings section and re-indexes the knowledge item; the message already in the Thread's Run keeps the names it had.
  - live_transcript transcribes the audio captured so far every 60 seconds while the capture runs (unless background work is deferred), so the Meetings section shows a growing transcript; the full transcription still runs after the capture.
  - The outputs Run is Unattended, its untrusted content has provenance `source_kind` "meeting" with the meeting id, and the trusted preamble holds no text from the meeting or the calendar.
- **Settings:** the Meetings section (spec: Meetings Settings Section) shows availability, the running capture, meetings with Transcribe now and speaker renaming, series opt-ins and, when allowed, manual capture of an application, all system output or the microphone.

## Risks / Trade-offs

- [Organization-level sign-off for production is refused or delayed] → The feature is off by default and enabled per Group by policy, so it stays limited to development and pilot Groups. Regions are modelled as Groups. The sign-off is organizational, analogous to the MS Teams recording policy, and is not enforced technically.
- [Parakeet Czech accuracy is insufficient on real organization meetings] → Spike 1.1 compares it with Whisper on real recordings. If Parakeet loses clearly on cs, the primary model is reopened through a change. Whisper turbo would not fit the client/desktop-host models budget without an amendment there, and it would need batch-overnight processing.
- [Diarization errors on single-mic conference rooms] → Allow user renaming and merging of speakers. Accept lower quality for room systems.
- [CPU load during the next meeting] → Transcription is batched after a meeting, deferred during an active capture and whenever client/desktop-host defers background work, and uses only half the cores.
- [Teams runs audio in a separate child process] → Capture the process tree. Detection is tested against the current Teams build in spike 1.2.

## Open Questions

- The spec sets policy.meeting_capture.require_notice to true by default. The organization-level process may decide to relax this per Group through policy; the spec is unaffected either way.
- Speaker naming beyond the confident case: matching several remote clusters to attendees with the user's confirmation, and local voice profiles learned from renames. Neither is in the spec; adding them needs a spec change (a confirmation Question, and voice profiles as personal data with forgetting).