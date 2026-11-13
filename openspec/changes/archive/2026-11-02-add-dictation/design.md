# Design

## Context

client/meeting-capture already has what Dictation needs: WASAPI capture of the default communications microphone (AudioCapture with CaptureSource mic), encrypted tracks (TrackWriter, DPAPI-wrapped AES keys), and the stt worker with Silero VAD, language identification and Parakeet-TDT-0.6B-v3 (stt.transcribe). Development builds use the deterministic test engine unless SKOWORK_STT_ENGINE=sherpa.

## Decisions

### D1. Dictation service in the host
- **Choice:**
  - DictationService (client/src/app or a small module next to meeting) owns one recording at a time.
  - dictation.start: refuses when policy.dictation.mode is off (policy_denied), when a meeting capture runs (busy) or when a dictation runs (busy). Otherwise it opens a TrackWriter at <data root>/capture/dictation-<uuid>.track (kind local, 16 kHz) and starts AudioCapture on the microphone. It returns {started_at}.
  - dictation.stop: stops the capture, closes the track, calls stt.transcribe {tracks: [path]} in the stt worker (user-started work, never deferred), joins the segments' text with spaces, deletes the file and returns {text, language}. An empty result returns text "".
  - dictation.cancel: stops and deletes.
  - A timer stops a recording after 5 minutes and leaves it ready for dictation.stop; the shell is told with the event dictation.changed {state}.
  - At start, the service deletes leftover dictation-*.track files.
- **Why:** the host already owns capture and the workers; the shell only asks for start, stop and cancel.

### D2. Availability in app.status
- **Choice:** app.status.dictation = {enabled (policy), available, reason}. reason is models_missing (no stt runtime or models), no_microphone (no active capture endpoint), meeting (a meeting capture runs) or empty. The models check is the presence of the files make_sherpa_engine needs, without loading them; development builds with the test engine count as available.

### D3. The Dictate button
- **Choice:**
  - An icon button with the microphone glyph (E720) in each composer's action row, next to Attach files. Name and tooltip "Dictate"; disabled with the reason as tooltip.
  - While recording: a Stop button (E71A, accent) with the elapsed time ("0:12") and a Cancel icon button. Escape in the composer cancels.
  - While transcribing: a progress label "Transcribing…"; the composer stays editable.
  - The text goes in at the caret with a separating space; the composer keeps the keyboard focus.
  - UI Automation announces "Recording", "Transcribing" and "Text inserted" through the live region the shell already has.

### D4. Policy key
- **Choice:** policy.dictation.mode, enum off|on, default on, merge scalar, guardrail max-level, like messaging.mode. The client's policy schema fixture is regenerated from the control plane's keys.

### D5. Dictation language
- **Choice:**
  - The setting dictation.language is "auto" (the default) or cs, de, en, pl or sk. It is stored with the other window settings (settings.get and settings.set) and chosen in Settings > General.
  - dictation.stop sends the chosen language to stt.transcribe. With "auto" it sends no language, and it sends fallback_language: the window's language, or en.
  - stt.transcribe: when the detected language is not supported and fallback_language is, the request runs again with that language.
- **Why:**
  - On a few seconds of speech, Whisper small's language identification can mistake Czech for a neighbouring language. On the demo client, one Czech dictation failed with unsupported_language.
  - Parakeet transcribes the European languages without being told which one, so the fallback still gives the spoken words.
  - A user who always dictates in one language can skip detection altogether.

## Risks / Trade-offs

- [Transcription takes time on slow laptops] → Parakeet runs about 10 to 20 times faster than real time on the baseline laptop (add-meeting-capture D1), so a 30-second dictation takes a few seconds.
- [The stt worker's model load adds latency to the first dictation] → accepted; the worker stays alive until its idle exit.

## Implementation notes (2026-11-02)

- DictationService is in client/src/meeting/dictation.cpp. It records into an encrypted dictation-<id>.skau track (kind local) under <data root>/capture. The RPCs are registered next to the meetings RPCs (register_dictation_rpc). The window cancels its recording when it closes, and so does shutdown.
- The availability check looks at the microphone and the speech models at most every 30 seconds. Without loading anything, it checks for the default communications capture endpoint and the files make_sherpa_engine opens.
- In the window, DictateControl sits in the composer's action row (Thread and new Thread: after Attach files; Chat: before Send). One recording per window. The text goes to the composer that started it, and is dropped (with "Recording discarded.") when that composer is gone.

## Acceptance (2026-11-02)

- Tests:
  - Dictation.StopTranscribesAndCancelDiscardsTheAudio, Dictation.RefusalsAndAvailability and Dictation.LimitAndLeftovers: the test engine and a fake microphone.
  - DictationSherpa.EnglishAndGermanSpeechBecomeText: the sherpa-onnx engine through the stt worker. On the models package's recordings it returned "Ask not what your country can do for you. Ask what you can do for your country." (en) and "Alles hat ein Ende, nur die Wurst hat zwei." (de), in about 11 seconds for both, including the model load.
  - ShellDictation.DictateInsertsAtTheCaretEscapeDiscardsAndAvailability.
  - ShellChats: the Chat composer has Dictate.
  - Policy tests: client and control plane.
- Demo client with SKOWORK_STT_ENGINE=sherpa: app.status.dictation is {enabled, available}. The new Thread composer shows the microphone button "Dictate" next to Attach files.
- Not checked yet: dictating by voice in English and Czech on the demo client (task 3.1). This needs the user's voice and their consent to record the microphone.
- Dictation language (D5), 2026-11-02:
  - Feedback from the user: English dictation works on the demo client. One Czech dictation failed with unsupported_language, because detection took the short Czech speech for a language that is not supported.
  - Added the fallback to the window's language and the Dictation language choice in Settings > General.
  - Tests: Dictation.LanguageChosenOrDetectedWithTheWindowLanguageAsFallback, and ShellDictation, which now covers the Settings choice.
  - Demo client: "Dictation language: Automatic" appears in Settings > General.
  - Czech by voice is still for the user to confirm (3.1).
- Acceptance 3.1 (2026-11-02, demo client, sherpa-onnx): the user dictated in English and in Czech and confirmed the text. The log shows 	ranscribed (cs). No dictation-* audio remains under the capture folder. Escape is covered by ShellDictation.
