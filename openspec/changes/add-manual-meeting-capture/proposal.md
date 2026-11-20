# Proposal

## Why

Meeting Detection only finds meetings in the listed apps: Teams and Zoom desktop, and browser windows whose title shows Jitsi or Meet. A Teams meeting joined in Edge or Chrome is never detected, so the user is never offered capture. The user needs to be able to start meeting capture themselves.

Manual Audio Source Capture already exists, but it does not cover this case:
- It is off unless policy.meeting_capture.allow_manual is true, and the default is false.
- Capturing an application records only that application's output. The user's own speech is missing, so the transcript, summary and follow-ups leave out everything the user said.
- A manual capture is not linked to the calendar. It gets no meeting title, no attendees for speaker naming and no meeting vocabulary.

## What Changes

- Manual capture is available whenever meeting capture is available. policy.meeting_capture.allow_manual now defaults to true and stays as the organisation's switch to turn it off.
- A manual capture of an application, or of all system output, also records the user's microphone as the local track, as for a detected meeting. A microphone-only capture stays a single track.
- A manual capture is matched to a calendar event the same way as a detected meeting.
- Starting a manual capture withdraws any pending "Start capture?" Question, because only one capture runs at a time.
- The new Thread composer gets a Record meeting button beside Dictate. A captured meeting becomes a Thread and a Knowledge source like mail, so the user starts it where they start other work, not in Settings. When exactly one application plays audio while the microphone is in use, one click records it. Otherwise the button lists the sources. The Meetings settings section keeps its source list.

Meeting Detection, the consent Question and series auto-capture are unchanged.

Out of scope:
- Detecting Teams in a browser automatically, for example with a default `msedge.exe|*Microsoft Teams*` entry. A Group can already add such an entry to policy.meeting_capture.apps.
- Capturing only one browser tab. Process loopback captures the whole browser process tree, so other tabs playing sound are recorded too.
- A tray menu entry or a keyboard shortcut for starting capture, and Record meeting in the composer of an existing Thread or a Chat.
- Merging Record meeting into the Dictate button. Dictation fills the composer and never sends anything, while a capture runs for the whole meeting and creates its own Thread.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/meeting-capture`: Manual Audio Source Capture becomes available by default, records the microphone beside an application or system output, matches the calendar, withdraws a pending consent Question, and can be started from the new Thread composer. These requirements still live in the in-flight change add-meeting-capture.

## Impact

- **Roadmap:** proposed position 48 (Knowledge), after stabilize-agent-tool-catalog.
- **Depends on:**
  - add-meeting-capture (manual capture, tracks, the consent Question, the calendar match, the Meetings settings section)
  - add-mail-calendar (the calendar events the match uses)
  - improve-meeting-capture-quality (echo removal and the meeting vocabulary now also apply to manual captures; no requirement of it changes)
- **Order of archival:** add-meeting-capture is archived first, because this change modifies requirements it adds.
- **Forward references:** none.
- **Code:** `client/src/meeting` (the policy default, manual capture start, the calendar match, the default source), the new Thread composer and the Meetings settings section in the native shell with their strings in cs, de and en, `control-plane/src/policy/keys.ts` (the default of meeting_capture.allow_manual), the policy schema fixture, and `client/tests/meeting` and `client/tests/shell`.
- **Policy:** the default of policy.meeting_capture.allow_manual changes from false to true. Groups that set it explicitly keep their value. Meeting capture itself stays off unless policy.meeting_capture.enabled is true.
- **Privacy:** no audio or text leaves the device because of this change. Consent stays explicit, because the user starts the capture. The recording indicator and policy.meeting_capture.require_notice apply as before.
