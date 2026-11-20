# Design

## Context

The motivation is in proposal.md. Capture, the tracks, the calendar match and the consent Question are built by add-meeting-capture (design D2 and D3). Echo removal and the meeting vocabulary from improve-meeting-capture-quality work on any meeting that has a local and a remote track and a matched calendar event, so they apply to manual captures once these exist.

## Decisions

### D1. Manual capture is on by default
- **Decision:** policy.meeting_capture.allow_manual keeps its name and meaning, and its default changes from false to true in the control plane's policy keys and in the client's policy defaults.
- **Why:** meeting capture as a whole is already off unless policy.meeting_capture.enabled is true, which a Group turns on deliberately. Within an enabled Group, manual capture is the only way to capture meetings that detection misses, and the user consents by starting it.
- **Alternatives considered:**
  - Remove the key, so manual capture follows policy.meeting_capture.enabled alone. Rejected: an organisation could no longer allow detected meetings while forbidding arbitrary sources such as all system output.
  - Keep the default false and set it to true per Group. Rejected: every Group that enables meeting capture would have to know to set it, or browser meetings would silently stay uncaptured.

### D2. The microphone is recorded beside an application or system output
- **Decision:** a manual capture opens the microphone as the local track unless the source is the microphone itself. The service decides this from the source alone, for a fresh start and for Continue after a restart alike. Detected meetings already record the microphone, so the rule becomes "every source except the microphone".
- **Why:** the main use is a meeting the user takes part in. Without the local track, the user's speech is missing and speaker separation, echo removal and the outputs are all wrong.
- **Alternatives considered:**
  - A "with microphone" option per manual capture. Rejected for now: one more choice at the moment a meeting starts, and the only case it helps is listening to a webinar without speaking. There the microphone track is silence, which voice activity detection drops, at about 115 MB per hour of encrypted PCM until transcription deletes it.
  - Record the microphone only for application sources. Rejected: "all system output" is the fallback when the user cannot tell which application plays the call, and the user's speech is needed there too.

### D3. The calendar match is the detection's match
- **Decision:** manual start uses the same match as Meeting Detection: the closest event within 10 minutes that has an online meeting link. It fills the title, event id, series id, attendees and meeting vocabulary the same way.
- **Alternatives considered:** let the user pick the event. Rejected for now: it adds a step, and the automatic match covers scheduled meetings. Ad-hoc calls get the source's title, as detected ad-hoc calls get the application's name.

### D4. A pending offer is withdrawn
- **Decision:** starting a manual capture declines every meeting in the offered or notice state and withdraws its Action item, as when the "Start capture?" Question times out.
- **Alternatives considered:** refuse the manual start while an offer is pending. Rejected: the user has just said what they want captured.

### D5. Record meeting in the new Thread composer
- **Decision:** the new Thread composer has a Record meeting button and a drop-down beside Dictate, shown while `app.status` reports `meetings.manual` (meeting capture available and policy.meeting_capture.allow_manual). A click asks the host for `meetings.sources`. When it names a default, the click records it at once; otherwise the sources open as a menu. `meetings.capture` returns the meeting's id and Thread, and the window opens that Thread, where the transcript, summary, decisions and follow-ups appear later. The button is disabled while `meetings.capturing` is true or a dictation runs. The Meetings settings section keeps its list.
- **Default source:** the processes with an active render session are grouped under the topmost ancestor with the same image name, so that a browser whose audio comes from a child process is one entry and the capture includes its whole tree. When the microphone is in use and exactly one application remains, it is the default. Otherwise there is none and the user chooses. This needs no window-title pattern, so Teams, Meet or Webex in any browser work alike.
- **Why:** a captured meeting is a source for the Thread and for Knowledge, like mail. The user starts it where they start other work, not in Settings, with one click in the common case.
- **Alternatives considered:**
  - Make it a mode of the Dictate button. Rejected: Dictate fills the composer and never sends anything, while a capture runs for the whole meeting and creates its own Thread. One button for both would make a click unpredictable.
  - Detect Teams in a browser by window title (`msedge.exe|*Microsoft Teams*`). Rejected as the main way: browser titles differ by meeting service and language, and the user asked for manual control. A Group can still add such an entry to policy.meeting_capture.apps.
  - Use the microphone's per-application consent store to see which application uses the microphone. Rejected for now: the detector reads only whether the microphone is in use at all, and the single-application rule covers the common case.
  - Offer it in every composer. Rejected for now: in an existing Thread or Chat it is unclear whether the meeting belongs to that Thread. The meeting gets its own Thread.

## Risks / Trade-offs

- [Two applications play audio during a call, for example a music player] -> No default; the list opens and nothing is recorded until the user picks.

- [Groups that enabled meeting capture now also allow manual capture without having chosen it] -> Manual capture needs the user to act and shows the recording indicator, and policy.meeting_capture.require_notice still applies. The change is called out in the proposal's Policy impact so administrators can set allow_manual to false.
- [Capturing a browser also records other tabs that play sound] -> Stated as out of scope in the proposal. The Meetings settings section's manual-capture text says that the whole application is recorded.
- [The calendar match picks a different meeting when two overlap] -> Same behaviour and limit as for detected meetings. The user can rename the Thread and the speakers.

## Acceptance

Results are recorded here as tasks complete.

- **3.1 (2026-11-20, test machine):** the copied `client/build/test` cache pointed at a toolchain path that does not exist here, so the test preset was configured into `client/build/test-local`; the build had no errors or warnings. `win_tests.exe`: 1,000 tests, 968 passed, 30 skipped, 2 failed. `ShellFeed.CtrlCWithTheComposerFocusedCopiesTheMessageSelection` passed when rerun alone (clipboard timing). `MeetingSherpa.VocabularyHotwordsAndRespelling` failed because the real speech engine returned no segments for the Czech fixture; it runs the worker, engine and respelling, none of which this change touches. All `MeetingService`, `MeetingCore` and `ShellMeetings` tests pass. `ui_tests.exe`: 104 passed. `policy_tests.exe`: 12 passed.
- **4.1, 4.2 (2026-11-20):** with Record meeting in the composer, the full client run is recorded in show-meeting-transcription-progress design Acceptance 4.1 (the same build). Two real Teams meetings in Edge were recorded and transcribed on the test machine with release build `0.1.1-dev.29.manualcapture`. Control plane: the policy tests (46) pass, including the schema-fixture check. In the full `npm test` run, `policy: an unchanged bundle is reissued with a new version after half its validity` failed once; it passes alone and in two runs of `test/policy.test.ts`, with and without this change.
