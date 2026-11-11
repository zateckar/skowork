# Proposal

## Why

Three weaknesses of meeting capture show up in ordinary use and none is covered by a spec today:

- A user without a headset gets every remote sentence twice. The microphone hears the loudspeakers, so the same words are transcribed on the local track and attributed to the user. The summary, decisions and follow-ups then credit the user with what others said.
- Names of people, organisations and internal terms are misheard, because transcription knows nothing about the meeting. These are the words the summary and client/knowledge-graph depend on most.
- A capture is fragile. Plugging in a headset mid-meeting ends the affected stream, and a client restart ends the capture even though the meeting goes on.

A comparison with the open-source meeting recorder Yogurt (FrigadeHQ/yogurt, MIT) on 2026-11-11 showed that all three are solvable on the device with modest means. This change adopts the ideas, not the code.

## What Changes

- Add echo removal. Local-track speech that repeats what the remote track said at the same moment is left out of the transcript, the outputs and the knowledge. The user can see what was left out and restore it.
- Add a meeting vocabulary. Attendee names, their organisations and the user's own terms guide transcription, and a recognised word that is one letter away from a vocabulary term takes its spelling.
- Make capture continue across audio device changes and stream failures, with the tracks kept aligned and trouble shown on the recording indicator.
- Make capture survive a client restart. The audio captured before the restart is kept, and the user is asked whether to continue while the meeting still runs.

Out of scope:
- Acoustic echo cancellation of the microphone signal, and any change to what the meeting application itself hears.
- A vocabulary for Dictation (client/dictation). It can reuse this mechanism through a later change.
- Vocabulary terms taken from client/knowledge-graph, shared vocabularies for a Group, or a policy-supplied term list.
- Reading speaker names from the meeting application's window, a compressed audio format, live captions, and the other ideas from the comparison.
- A new policy key. Nothing here changes what may be captured, who is asked, or how long audio is kept.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/meeting-capture`: echo removal, the meeting vocabulary, capture across device changes, and capture across a client restart. Most of this capability's requirements still live in the in-flight change add-meeting-capture; this change only adds requirements.

## Impact

- **Roadmap:** proposed position 44 (Hardening, Knowledge), after add-api-connectors.
- **Depends on:**
  - add-meeting-capture (capture, tracks, the transcription worker, the consent Question, the recording indicator, the Meetings settings section)
  - add-mail-calendar (attendees of the matched calendar event)
  - add-work-inbox (the "Continue capture?" Question as a standalone Action item)
  - improve-skowork-and-knowledge-control (the speech package of client/speech-models, which this change does not alter)
- **Order of archival:** add-meeting-capture is archived first, so that this change's requirements merge into a complete capability.
- **Forward references:** none.
- **Code:** `client/src/meeting` (capture streams, the track files, the transcription worker's pipeline, the service's merge step and startup), the Meetings settings section in the native shell, the stored meeting record in `app.db`, and `client/tests/meeting`.
- **Artifacts and budgets:** no new model and no new third-party code. The speech package and the models budget of client/desktop-host are unchanged. Transcription must stay inside the Transcription Resource Budget with the vocabulary in use; spike 1.1 measures it.
- **Privacy:** no audio or text leaves the device because of this change. The vocabulary is used only for transcription and never enters a model context except as words of the transcript.
