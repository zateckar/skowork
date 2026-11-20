# Proposal

## Why

On 2026-11-20 the user recorded two Czech Teams meetings of about 20 minutes on the test machine, and found the transcripts hard to use. The first three minutes of one transcript show four kinds of error:

- Short fragments came out in English, as words that nobody said: three-word phrases between Czech sentences, attributed to a remote speaker.
- A Czech sentence came out in Russian, in Cyrillic.
- Two of the user's segments repeated, slightly garbled, what a remote participant had said a moment earlier. The laptop's loudspeakers reached the microphone, and echo removal (improve-meeting-capture-quality) did not catch them because the garbled words differed too much.
- Product and team names were misheard. The test machine has no calendar, so the meeting vocabulary was empty.

The cause of the first two is in the pipeline (add-meeting-capture design D4). The meeting's language is identified once, but Parakeet, which writes the text, cannot be told the language: it chooses one for every piece of speech it decodes. Voice activity detection cuts speech at every pause of 0.4 seconds, so many pieces are a word or two long, and Parakeet guesses their language from almost no context.

## What Changes

- Speech is decoded in context. Pieces of one track separated by short pauses are decoded together, up to 20 seconds, instead of one by one.
- The transcript keeps to the meeting's language. A segment written in another script than the meeting's language, or a short segment recognised in another language, is decoded again with the meeting's language set. What still does not fit is shown as "(unclear)" and left out of the outputs and the knowledge item, instead of appearing as foreign words.
- Echo removal catches near copies. The two leaked segments break the existing Echo Removal From The Local Track requirement; the word match is made tolerant of the garbling that loudspeaker echo causes. This is a fix within that requirement, not a new rule.
- The user can choose the meeting's language for a capture from the Record meeting list, and transcribe a finished meeting again in another language. Today the language can only be detected.

Out of scope:
- A different or larger speech model. Whisper large-v3-turbo does not fit the models budget and is too slow on the baseline laptop (add-meeting-capture design D1). Change this only if spike 1.2 shows the measures here are not enough.
- Correcting the transcript with a language model. It could invent words that were never said, and the transcript is the record.
- A vocabulary from sources other than the calendar and the user's terms.
- Meetings held in two languages at once. Long segments in another supported language are kept as spoken; only short fragments and other scripts are decoded again.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/meeting-capture`: decoding in context, keeping to the meeting's language, choosing the meeting's language, and transcribing again. Echo Removal From The Local Track (in-flight change improve-meeting-capture-quality) is fixed, not changed.

## Impact

- **Roadmap:** proposed position 51 (Knowledge), after show-meeting-transcription-progress.
- **Depends on:**
  - add-meeting-capture (the pipeline, Local CPU Transcription, the meeting Thread)
  - improve-meeting-capture-quality (echo removal and the meeting vocabulary)
  - improve-skowork-and-knowledge-control (the supported languages: Czech, English and German)
  - add-manual-meeting-capture (the Record meeting list)
- **Order of archival:** after add-meeting-capture and improve-meeting-capture-quality.
- **Forward references:** none.
- **Code:** the sherpa-onnx engine in `client/src/meeting` (decoding units, the language check, decoding again with Whisper small), the echo word match, the service and store (the chosen language, transcribing again), the composer's Record meeting list and the meeting Thread in `client/src/shell` with strings in cs, de and en, and the tests.
- **Budgets:** no new model; Whisper small is already in the speech package for language identification. Decoding again costs time only for the segments that need it; spike 1.2 measures it against the Transcription Resource Budget.
- **Privacy:** no change. Nothing leaves the device because of this change.
