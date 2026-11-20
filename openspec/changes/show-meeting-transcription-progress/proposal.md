# Proposal

## Why

On 2026-11-20 the user recorded two Teams meetings of about 20 minutes on the test machine. After each meeting ended, it took several minutes before the summary appeared in the meeting's Thread. During that time the Thread showed nothing, so the user could not tell whether anything was happening.

The log of those meetings shows why the wait is long. With policy.meeting_capture.live_transcript on, each live pass transcribed all the audio captured so far, so the passes grew longer as the meeting went on (1.5 minutes apart at the start, more than 2.5 minutes at the end). After the capture ended, the full transcription ran once more from the start (2 to 3 minutes for 20 minutes of audio), and only then did the summary Run start.

## What Changes

- The meeting's Thread shows where the meeting is: recording, waiting for transcription and why, transcribing with progress and an estimate of the time left, writing the summary, done, or failed with the reason. The line updates while the user watches.
- With live transcription on, each pass transcribes only the speech captured since the previous pass and keeps the earlier text. When the capture ends, only the rest of the speech is transcribed, and speakers are separated once over the whole meeting. The transcript and the summary follow the end of the meeting closely instead of after a full second transcription.
- Without live transcription (the default), transcription still starts when the capture ends; the progress line is the remedy there.

Out of scope:
- Transcript quality (languages, wrong words, echo): change improve-transcript-language.
- Showing the transcript itself growing in the Thread during the meeting.
- The way the summary Run's message shows the transcript in the Thread.
- Progress for Dictation (client/dictation), which is short.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/meeting-capture`: a progress line in the meeting's Thread, and live transcription that transcribes only new speech. Batch Transcription By Default still lives in the in-flight change add-meeting-capture.

## Impact

- **Roadmap:** proposed position 50 (Knowledge), after remove-ci-spec-validation.
- **Depends on:**
  - add-meeting-capture (the meeting states, live transcription, the meeting Thread, the stt worker)
  - add-manual-meeting-capture (Record meeting opens the meeting's Thread, where the progress line shows)
  - add-work-inbox (the Thread view the line is part of)
- **Order of archival:** after add-meeting-capture, whose Batch Transcription By Default this change modifies.
- **Forward references:** none.
- **Code:** `client/src/meeting` (the service's live and final passes, the progress state, the worker request for part of a track and for diarization alone), `client/src/app` (status of the meeting for its Thread), the Thread view in `client/src/shell` with strings in cs, de and en, and the tests.
- **Budgets:** the live passes use less CPU than today, because each audio second is transcribed once. The Transcription Resource Budget is unchanged.
- **Privacy:** no change. Nothing leaves the device because of this change.
