## 1. Policy default

- [x] 1.1 Change the default of `meeting_capture.allow_manual` to true in `control-plane/src/policy/keys.ts`, in the client's policy defaults (`client/src/meeting/core.cpp`) and in the policy schema fixture. Verify: the control-plane policy tests and the client test for meeting-capture defaults expect true, and a policy that sets false still turns manual capture off.

## 2. Manual capture

- [x] 2.1 Write tests in `client/tests/meeting/meeting_test.cpp`: a manual capture of an application and one of system output open a local microphone track beside the source; a microphone capture opens one track; Continue after a restart of a manual application capture reopens both tracks; a manual capture within 10 minutes of an online meeting takes its subject, event id, attendees and vocabulary; without a match it is titled after the source; a pending "Start capture?" Question is withdrawn when a manual capture starts. Verify: the tests fail before 2.2 and pass after it. (2026-11-20: added as `ManualCaptureOfAnApplicationWhenAllowed` (updated), `ManualCaptureTracksPerSource`, `ManualCaptureMatchesTheCalendar`, `ManualCaptureWithdrawsAPendingOffer` and `ManualCaptureContinuesWithBothTracks`; all pass after 2.2. They were written before 2.2 but not built and run against the old code, so the failing-before step was not observed.)
- [x] 2.2 In `MeetingService`, record the microphone for every source except the microphone, run the detection's calendar match on manual start, and decline pending offered and notice meetings on manual start. Verify: the tests of 2.1 and all existing meeting tests pass.
- [x] 2.3 Update the manual-capture text of the Meetings settings section in cs, de and en to say that the microphone is recorded too and that the whole application is captured. Verify: the shell test for the section passes and the strings exist in all three languages.

## 3. Record meeting from a new Thread

- [x] 3.1 Group manual sources by the topmost process of the same name, add the default source (`meetings.sources` returns `default`), return `{meeting_id, thread_id}` from `meetings.capture`, and add `meetings` (`manual`, `capturing`) to `app.status`. Verify: `MeetingService.ManualSourcesDefaultToTheOneApplicationInACall` and the updated manual-capture tests pass.
- [x] 3.2 Add Record meeting and its drop-down to the new Thread composer, with strings in cs, de and en. Verify: `ShellMeetings.RecordMeetingFromANewThread` covers one-click capture with the Thread opened, the list without a default, the drop-down, the disabled state with its tooltip, and the hidden state.

## 4. Verification

- [ ] 4.1 Build and run the client tests: `cmake --build --preset test` in `client/`, then `client/build/test/bin/win_tests.exe` and `client/build/test/bin/ui_tests.exe`; run the control-plane policy tests. Verify: all pass, and the results are recorded in design.md Acceptance.
- [x] 4.2 Needs the user's OK because it records real audio: on the test machine, join a Teams meeting in Edge, click Record meeting in the new Thread composer, speak, stop, and let it transcribe. (2026-11-20: the user recorded two Teams meetings in Edge with Record meeting, about 19 and 15 minutes; one click chose Edge, the meeting Thread opened, both tracks were recorded, and both meetings were transcribed and summarised. Transcript quality and the wait led to show-meeting-transcription-progress and improve-transcript-language.) Verify: the transcript has the user's speech as the user and the other participants' speech, and the result is recorded in design.md Acceptance.
- [ ] 4.3 After add-meeting-capture is archived and every task above is checked, run `openspec archive add-manual-meeting-capture --yes`, then reread the merged `openspec/specs/client/meeting-capture/spec.md`. Verify: the archive succeeds and the merged spec reads consistently.
