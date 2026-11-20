# Design

## Context

The motivation is in proposal.md. The pipeline is add-meeting-capture design D4: Silero VAD per track (0.4 s minimum silence, at most 20 s per segment), diarization of the remote track, language identification by Whisper small on up to three windows, then Parakeet-TDT-0.6B-v3 in batches of up to 8 segments and 20 s of audio. Parakeet is multilingual and takes no language setting; it picks a language per decoded piece. Whisper small takes a language setting and is already loaded for language identification.

Evidence (2026-11-20, test machine, two Czech Teams meetings in Edge, loudspeakers and laptop microphone, no calendar): in the first three minutes of one transcript, five one- to three-word segments came out as English words that were not said, one Czech sentence came out in Cyrillic, two local segments were near copies of remote speech, and product names were misheard. Longer Czech sentences were largely right. The audio was deleted after transcription (policy keep_audio off), so these meetings cannot be decoded again; spike 1.1 records new material.

## Decisions

### D1. Decoding units of up to 20 seconds
- **Decision:** VAD keeps its settings; the engine then joins consecutive spans of one track whose gap is under 1 s into a decoding unit, while the unit stays at most 20 s (the batch limit that keeps Parakeet inside the 2 GB Job limit, D4 Memory). The unit's audio, pauses included, is decoded once; Parakeet's token timestamps split its words back onto the spans (a word goes to the last span that starts at most 100 ms after its first token), so the transcript keeps per-span times and the diarization assignment per span is unchanged. A unit whose result has no token timestamps gives its whole text to its first span. Whisper (forced in development builds) still decodes span by span. Code: `join_spans` and `split_by_time` in `client/src/meeting/core.cpp`, used by `recognize` in `sherpa_engine.cpp`.
- **Alternatives considered:**
  - Raise VAD's minimum silence to 1 s. Rejected: it changes segment timing everywhere, merges turns of different remote speakers into one segment before diarization, and does not help when a pause is just above the threshold.
  - Decode each span with its neighbours as left context and keep only its own words. Rejected: it decodes most audio two or three times.

### D2. The language check
- **Decision:** after decoding, each segment is checked against the meeting's language:
  - **Script:** cs, de and en are Latin-script; a segment with letters outside the Latin script fails.
  - **Short fragments:** a segment of at most five words fails when at least half of its words are in a list of frequent function words of another supported language and none is in the meeting language's list (for example, in a Czech meeting, "you", "the", "are", "we", "yeah" without any Czech function word). A word in both lists counts for the meeting's language: "i" is Czech as well as English, so "I'm …" never fails a Czech meeting. Words common to all three ("ok", "no", "super") are in no list. The lists, about 100 words per language, are written by us in `client/src/meeting/core.cpp`, not taken from a third-party lexicon. Five words, not three: the fragments of 2026-11-20 included four-word ones ("So we gonna hold").
  - Failed segments are decoded again, one by one, with Whisper small with the meeting's language set, after Parakeet is unloaded. A result that is empty, fails again, or matches a known silence hallucination of Whisper ("Titulky vytvořil…", "Thanks for watching") becomes "(unclear)". If Whisper cannot start, the failed segments become "(unclear)" rather than stay foreign.
  - **As built:** no context padding. Whisper gives no reliable word times, so words from padding could not be removed again; the segment alone with the language set was enough for the cases above.
- **Alternatives considered:**
  - Language identification per segment with Whisper. Rejected: unreliable on speech under 2 s, which is where the errors are, and it costs an encoder pass per segment.
  - Decode everything with Whisper small with the language set. Rejected: noticeably worse Czech than Parakeet on long speech (D1 of add-meeting-capture) and about 4 times slower.
  - Drop failed fragments silently. Rejected: the user should see that something was said there.

### D3. Echo near copies
- **Decision:** the echo rule's word match (improve-meeting-capture-quality D1) compares normalised words; for a local segment of 4 or more words it now also accepts a match when its text, lower case without diacritics and punctuation, is at least 60% similar (edit distance over length) to the best word-aligned window of about its length in the remote speech that overlaps it or ended at most 1 second before. The audio evidence condition is unchanged, so without correlated audio nothing is echo. The two leaked segments of 2026-11-20 had full audio overlap and garbled words. Code: `text_similarity` and `mark_echo` in `client/src/meeting/core.cpp`.
- **Alternatives considered:** lower the word-match threshold. Rejected: it would start removing the user's genuine read-backs, which the rule's Read-back scenario protects.

### D4. Choosing the language and transcribing again
- **Decision:** the Record meeting list gains language rows (Automatic and the supported languages, the current one checked). Choosing one calls `meetings.set_language`, which the host remembers in `meeting_state` (migration 2100); every manual capture then starts with that language, used without detection, and `meetings.sources` reports it. A meeting's Thread offers "Transcribe again in…" while its audio is kept (policy keep_audio) and it is done or failed; `meetings.transcribe_again` sets the language, clears the transcript, and queues the meeting as user-started. The new transcript replaces the knowledge item when it is indexed, and a new summary Run answers in the Thread; the earlier summary message stays above it in the Thread's history.
- **Why the host remembers it:** a one-click Record meeting has no list open, and the setting belongs with meeting capture, not with the window.
- **Alternatives considered:** a language setting in Settings for all meetings. Rejected: the right language differs by meeting, and Dictation already has its own setting.

## Risks / Trade-offs

- [The function-word check flags genuine short English replies in a Czech meeting, such as "OK, thanks"] -> They are decoded again in Czech, which keeps understandable words; spike 1.2 counts false flags, and the list excludes words both languages use ("OK", "super", "no").
- [Whisper small hallucinates on short or quiet segments] -> Context padding of 2 s, the hallucination list, and "(unclear)" as the fallback.
- [Joined units change the transcript's segment times] -> Token timestamps split units back onto the VAD spans; a test checks times against the fixture.
- [Decoding again costs time] -> Only failed segments; spike 1.2 records the share and the time on the test machine.

## Acceptance

Results are recorded here as tasks complete.

- **Client tests (2026-11-20):** as recorded in show-meeting-transcription-progress design Acceptance 4.1, which ran with both changes. The 60-minute fixture meeting (English) triggered no decoding again; the language check has not yet met real Czech audio here (spikes 1.1 and 1.2).
