# Proposal

## Why

Transcripts of Teams meetings contain only the user. On 2026-11-24 the remote track of a 33-minute Teams meeting was digital silence from start to end, while the local track held the user's speech. A live Teams call measured the same moment on every capture path: process loopback of the outermost `ms-teams.exe`, of the `ms-teams.exe` child and of the WebView2 process that own the active audio sessions all delivered digital silence (peak 1), while loopback of the output device Teams played to (a Jabra headset) delivered the call at about -24 dBFS. Process loopback (add-meeting-capture design D2) does not receive the current Teams client's call audio. Task 1.2 of add-meeting-capture, which was to prove process loopback against current Teams, had never been run.

## What Changes

- While the remote track records a meeting app by process loopback, the client also measures, without recording it, the loopback of the output device on which the app has an active audio session.
- When that device has carried 3 seconds of sound while process loopback has delivered only digital silence since the start of the capture, the remote track switches to the loopback of that device for the rest of the capture, and follows the app to another output device.
- Once process loopback delivers sound, it is known to work for the app and nothing is measured any more.

Out of scope:
- Separating other applications' sound on the same output device after a switch. While the meeting app plays to a headset this is the call audio; other sounds on that device are recorded too.
- Detection, consent, notice and transcription, which are unchanged.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- client/meeting-capture: Remote Audio From The Output Device (added).
