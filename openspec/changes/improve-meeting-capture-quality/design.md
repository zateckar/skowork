# Design

## Context

The motivation is in proposal.md. The capture and transcription pipeline is the one of add-meeting-capture (design D2 and D4): two encrypted tracks (`local` from the microphone, the remote track from process loopback), a worker that runs VAD, diarization and Parakeet per track, and a merge by time in the service (`parse_segments`, `name_speakers`).

What the code does today (read 2026-11-11):
- The microphone is the default communications endpoint. A stream error ends that capture thread (`AudioCapture::run`), and nothing reopens it.
- On startup a meeting in state `capturing` becomes `pending` with `ended_at` set to now (`MeetingService::start`). The track files are written in sealed chunks of about one second, and the reader already tolerates a missing end (`DecodedTrack::truncated`).
- The worker decodes with greedy search and no hotwords. The vendored sherpa-onnx C API has `hotwords_file`, `hotwords_buf` and `hotwords_score` on the offline recognizer; whether the Parakeet-TDT model honours them is unproven.

The ideas come from a comparison with Yogurt (FrigadeHQ/yogurt, MIT). No code is taken from it; its thresholds are used as starting values and re-measured on our fixtures.

## Goals / Non-Goals

**Goals:**
- A transcript without duplicated remote speech when the user has no headset, without losing the user's own words.
- Correct spelling of the meeting's people, organisations and the user's terms.
- A capture that survives a device switch and a client restart.

**Non-Goals:**
- Cleaning the audio itself. The tracks are stored as captured.
- Any new model, library or policy key.
- Dictation.

## Decisions

### D1. Echo is removed after transcription, by words and audio together
- **Words:** a `local` segment is compared with the remote segments that overlap it in time or ended at most 1 second before it started. Text is lowercased and stripped of punctuation. The share of the local segment's words found in the remote text must reach 60% for 8 or more words and 80% for 6 or 7 words; shorter segments must match nearly whole. A long segment is also compared with neighbouring remote segments joined, because VAD cuts the two tracks at different pauses.
- **Audio:** the worker computes, for each `local` segment, how closely the microphone's loudness envelope follows the remote track's over the segment's span, at the lag found for the meeting (the loudspeaker-to-microphone delay, searched between 0 and 300 ms on the first minutes of overlap). The worker returns this as `echo` evidence on the segment: `correlated` and `quiet` (no louder than the leak usually is). The worker already holds both tracks' PCM, so this costs one pass over the audio and no model.
- **Audio, as implemented:** loudness envelopes in 20 ms frames; the remote track counts as sounding above an RMS of 100. The lag (0 to 300 ms) is the one at which the microphone follows the remote track best over the first 10 minutes. "Follows" is a rank (Spearman) correlation, not a plain one: frames where the user talks over the remote side are far louder than the leak and would swamp a plain correlation. A meeting whose best correlation is under 0.3 has no echo path. The usual leak is the median ratio of microphone to remote loudness at that lag. A segment is `correlated` when the remote track sounds in at least half of its frames and the rank correlation over them reaches 0.5. It is `quiet` when its median ratio is at most 2.5 times the usual leak.
- **Rule:** a segment is echo when the word rule holds and the audio is `correlated`. A segment shorter than 6 words additionally needs `quiet`. A meeting in which no span is `correlated` (a headset) has no echo at all, whatever the words say.
- **Storage:** an echo segment is kept in the meeting's stored segments with `echo: true`. Everything that builds the transcript (the Thread's Run message, the Meetings section, `knowledge_turns`) skips such segments. Restore clears the flag and re-indexes, the same path as a speaker rename.
- **Spike 1.3 (2026-11-11, development workstation, Jabra Link 370 headset as the only microphone, the test speech played by a separate process):**
  - Loudspeakers (S/PDIF), headset on the desk: 1 of 5 remote utterances, the loudest, came back as a local segment ("Ende. Nur die Wurst hat zwei."). With the user speaking, all six user lines were kept, and none of the loudspeaker speech came back as text. The six lines were a short reply, a read-back 1.6 s after the remote line, two interruptions over remote speech, "Okay, thanks", and a closing sentence.
  - Headset worn, speech in the earphone: five user lines and no echo.
  - Reading: the headset's own noise suppression removes most echo, so with a headset like this the rule mostly has to keep the user's lines. The overlap window (1 second) keeps the read-back. A realistic echo share needs a laptop's built-in microphone and speakers and is not measured yet.
  - Implementation check (2026-11-11, `MeetingEcho.EvidenceOnTheSpikeRecordings`): on both recordings no local span is `correlated` and all 11 user lines are kept. The take with the one echo line was overwritten, so a positive case on real audio is still open; the synthetic test `MeetingEcho.EvidenceFollowsTheLoudspeakerLeak` covers it.
- **Alternatives considered:**
  - Acoustic echo cancellation on the microphone signal before transcription. It needs a vendored DSP library and tuning per device, and it would change the stored audio. Rejected for this change; the non-goal stands.
  - The Windows communications-stream echo cancellation. It depends on the audio driver and is absent on many devices, so it cannot be relied on.
  - Words only. It drops the user's honest "yes, I can send it" whenever a participant said something similar. Rejected.
  - Muting the local track while the remote track has speech. It loses everything said while both talk. Rejected.

### D2. Vocabulary: hotwords in the decoder where the model supports them, respelling always
- **Sources:** attendee display names of the matched calendar event (given and family name as separate terms); the organisation from each attendee's address (`person@company.example` gives "Company"; public mail providers and labels shorter than 4 letters are skipped; for `co.uk`-style domains the label before it is used); the user's terms, stored with the user's settings in `app.db`. The user's own name is included.
- **Decoder:** the service passes the vocabulary with the transcription request, and the worker decodes with modified beam search (4 active paths) and the vocabulary as hotwords at score 2.0. Decided by spike 1.1 (below).
- **Hotword setup:** Parakeet's `tokens.txt` is a SentencePiece table, and sherpa-onnx only encodes plain-text hotwords for it with `modeling_unit = "bpe"` and a `bpe_vocab` file (each token with its score). The package has no such file, so the worker writes one from `tokens.txt` into its temporary folder on first use (token and `-<id>` per line, special `<…>` tokens left out). Without it, sherpa-onnx skips every hotword with a log warning and decodes as if there were none. The speech package is unchanged.
- **Respelling:** after decoding, a word of 5 or more letters that starts with a capital is compared with each vocabulary term, both whole and with an ending of up to 3 letters removed. When either form is one edit away from a term (a swap of neighbouring letters counts as one edit), the word takes the term's spelling and keeps its ending. Words equal to a term apart from case take the term's case. The ending rule exists because cs and de inflect names: the vocabulary holds "Oldřich", but the transcript needs "Oldřicha".
- **Endings are not edits (found while implementing):** a word that differs from a term only in its last letter, or by one letter added at the end, is an inflected form and is left as it is. Without this, "Svobodové" (genitive of "Svobodová") and "Zatecko" (vocative) would be "corrected" to the nominative, and "Nováka" would lose its ending. A word that is a term plus an ending of up to 3 letters keeps the ending and takes the term's spelling of the stem.
- **The service applies it:** respelling and echo marking run in the service after the worker returns, on the vocabulary the service sent. The service sends the meeting's terms (stored with the meeting at detection, so a later calendar change does not alter them), the user's own name and the user's terms.
- **Spike 1.1 (2026-11-11, development workstation i7-12700, sherpa-onnx 2.0.1, fixtures `parakeet-cs/de/en`, `whisper-0/1`, 43 s, about 150 words):**
  - Reference (user-confirmed): "Břetislav … syn knížete Oldřicha". Greedy gives "Přetislav … syn kníže Olřicha".
  - Beam 4 with hotwords "Břetislav" and "Oldřich" at 2.0: "Oldřicha" is corrected. "Přetislav" is not. At 3.0 it becomes "Bředislav", closer but still wrong. "kníže" for "knížete" is grammar, outside any vocabulary.
  - A list of 20 terms (the two names and 18 never said) at 2.0: no change to the de and en fixtures. The term "Zatecka" turns "Přetislav" into "Zatislav", so a term can replace a similar-sounding unknown word.
  - Scores of 3.0 and above damage unrelated text: at 3.0 "zwei" became "2" and "Vád" became "Zvád". At 5.0 English lines turned into "Country Club" and "Fischer Becked Sol".
  - Cost, five minutes of audio decoded 7 times: greedy RTF 0.094 at 10 threads and 0.081 at 2 threads. Beam 4 with 20 hotwords: 0.100 and 0.077. Peak private memory about 965 MB in each case. Beam search with hotwords costs no measurable time or memory here; the baseline laptop run (add-meeting-capture task 4.5) covers it.
  - Kept as the decision: beam 4 at 2.0, plus respelling. Hotwords alone do not reach the spec's "Zatecka" scenario for names the model splits differently, so respelling stays.
  - In the worker (2026-11-11, `MeetingSherpa.VocabularyHotwordsAndRespelling`): the worker decodes per VAD segment, not the whole file as the spike did. There the model hears "Zoislav … knížete Olřicha". Beam search fixes "knížete"; the hotword does not fix "Olřicha"; respelling does. "Zoislav" is two edits from "Břetislav" and stays. With "Zatecka" in the vocabulary, "Zatislav" does not appear. The test prints the decoded text, so a change of score or model shows.
  - If sherpa-onnx refuses the recognizer with hotwords, the worker creates it again without them (greedy search), so the vocabulary never costs a transcript.
- **Trust:** attendee names and addresses are untrusted content. They reach only the decoder and the respelling step. The trusted preamble of the outputs Run still holds no text from the meeting or the calendar (add-meeting-capture D6), so the model sees vocabulary terms only where the transcript contains them.
- **Alternatives considered:**
  - Telling the summarising model how names are spelled. It would put calendar text into a trusted prompt. Rejected.
  - Respelling against a dictionary of everyday words, as Yogurt does for English. It needs word lists for cs, de and en inside the package. Rejected for size; the capital-letter and length rule is used instead, and task 1.2 measures its error rate.
  - Terms from client/knowledge-graph. Useful, but it needs ranking and a size limit decided by that capability. Left for a later change.

### D3. Streams are reopened by the capture thread
- The capture thread checks once a second whether the default endpoint it should follow has a different id, and treats a failed call on the stream the same way: it stops the client, opens the current default, and goes on writing to the same track writer. The existing rule that fills the track up to wall-clock time with silence keeps the tracks aligned.
- The loop is `supervise()` in core, with the stream as callbacks, so a synthetic stream on a test clock drives it (`MeetingCapture.SupervisedStreamReopensAndReportsTrouble`). The first open still fails the capture as before; only later failures are retried.
- The indicator grows a second line in amber ("Not recording: microphone") while a track is in trouble. The service polls the recorders on its 2-second tick and changes the indicator only when the set of affected tracks changes.
- The microphone follows the default communications capture endpoint. The remote track of a detected meeting is a process loopback and does not depend on the output device; only a manual "all system output" capture follows the default render endpoint.
- Reopening is tried every second for 30 seconds, then every 10 seconds. The capture reports per-track trouble to the service, and the indicator draws a warning mark with the track's name.
- **Alternatives considered:**
  - Device notifications from the endpoint enumerator. They arrive on a COM thread and add a callback object with its own lifetime; a one-second check in a thread that already wakes ten times a second is simpler and fast enough for the 5-second requirement.
  - Ending the capture and asking the user. It loses the meeting for a plugged-in headset. Rejected.

### D4. A restart continues the meeting in new part files
- **Before the restart:** nothing changes. Chunks are sealed about once a second, which is what the one-second requirement needs; task 4.1 pins this with a test.
- **On startup:** a meeting found in state `capturing` goes to a new state `interrupted` with the interruption time, instead of `pending`. While a meeting is `interrupted`, a detection of the same application whose matched calendar event is the same (or, without an event, any detection of that application) within 10 minutes raises "Continue capture?". Continue starts new track files for the same meeting, named as part 2 (and so on), whose header start time places them on the meeting's clock. Finish, the 2-minute expiry, or 10 minutes without a detection moves the meeting to `pending`.
- **Reading:** the worker reads a track's parts in order and puts silence between them according to their start times, so the pipeline sees one track. A part whose start lies inside the audio before it (a clock step) follows that audio directly.
- **The interruption time** is read from the track files: the header's start time plus the samples in the sealed chunks. File times are not used; they follow a different clock from the meeting's.
- **Storage:** the `meetings` table checks its states, so migration 1700 rebuilds it with `interrupted` added, and adds the `vocabulary` column and the `meeting_terms` table (the user's terms).
- **Consent:** the user consented to this meeting and confirmed the participant notice before the restart. The Question after a restart is asked even for an opted-in series, because the client cannot know why it stopped.
- **Alternatives considered:**
  - Appending to the existing file. The per-capture key is in the header, so it is possible, but a torn last chunk would have to be cut off first, and a failure while doing so risks the audio already captured. Parts never touch a finished file.
  - Resuming without asking. Capture after an unexplained stop should be visible to the user. Rejected.

## Risks / Trade-offs

- [Echo removal hides something the user said] -> Both words and audio must match, short segments need the stricter rule, and the Meetings section shows the count and restores any segment. Task 2.1 measures it on fixtures; a single wrongly hidden user segment in the fixture set fails the test.
- [Echo is transcribed differently on the two tracks, so the word rule misses it] -> The segment stays in the transcript, which is today's behaviour. Task 1.3 records the share removed on a real loudspeaker recording.
- [A hotword replaces an unknown word that sounds similar ("Přetislav" became "Zatislav" in spike 1.1)] -> The score is fixed at 2.0, the vocabulary holds only the meeting's attendees, their organisations and the user's terms, and the Meetings section shows the transcript for correction. Task 3.3 adds the spike's case as a regression test, so a change of score or model shows its effect.
- [Beam search breaks the resource budget on the baseline laptop] -> Spike 1.1 measured no extra cost on the development workstation (RTF 0.077 against 0.081 for greedy at 2 threads). add-meeting-capture task 4.5 measures the 60-minute meeting on the baseline laptop with this decoder; if it misses the 0.5 budget, hotwords are switched off and respelling alone remains.
- [Respelling with endings changes an ordinary word that ends like an inflected name] -> Only capitalised words of 5 or more letters are changed, the stem must still be one edit from a term, and a difference in the last letter alone is never an edit. Task 1.2 measures it on a larger corpus.
- [Respelling changes an ordinary capitalised word, most likely in German, where nouns are capitalised] -> Only words of 5 or more letters at one edit are changed. Task 1.2 counts wrong respellings on the cs, de and en fixtures; if German exceeds 1 per 1,000 words, respelling is limited to exact case fixes for German and this section records it.
- [A stream that reopens again and again floods the log or the indicator] -> One log line per change of state, and the indicator shows a state, not events.
- [The "Continue capture?" Question appears for a different meeting in the same application] -> The calendar event must match where there is one, and the window is 10 minutes. Without an event the user sees the meeting's title in the Question and can choose Finish.

## Open Questions

- Whether the user's terms should also come from the product glossary of a Group. Not in the spec; it needs a policy key owned by this capability and a change.

## Acceptance

- 2026-11-11, task 5.1, development workstation: `ui_tests` 70 of 70 passed. `win_tests` 782 of 788 passed, 5 skipped. The one failure, `KnowledgeImpactPreview.AnalyzedImportIsRecordedAsPreviewedAndPendingAnalysisDoesNotBlockIt`, is in client/knowledge-graph code that another change was editing in the same checkout at the time, not in this change. All meeting, dictation and shell tests passed, including the sherpa-onnx tests on the models package. The meeting fuzz target ran 200,000 mutated inputs without a crash.
- Task 4.5 (real devices: switching the default microphone, ending the client during a capture) is not run yet.
