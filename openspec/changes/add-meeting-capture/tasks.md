# Tasks

## 1. Validation spikes

- [ ] 1.1 Benchmark Parakeet-TDT-0.6B-v3 int8 (sherpa-onnx) against whisper.cpp large-v3-turbo q5 (reference only) and the Whisper-small q5 fallback on the baseline laptop, using about 5 hours of consented real cs/de/en meeting audio. Record word error rate, real-time factor, CPU%, peak RAM and battery drain per hour in design.md
- [ ] 1.2 Prove WASAPI process-loopback capture of the current Teams, Zoom and Chrome (Jitsi) builds, with separate microphone capture. Verify that other application sounds are excluded
- [ ] 1.3 Evaluate sherpa-onnx diarization on the remote track for meetings with 2–8 participants. Record the diarization error rate
- [x] 1.4 Record the CC-BY-4.0 attribution for the speech model in licence notices. Verify that it is visible in About (client/desktop-host Third-Party Notices, add-client-foundation 3.7)

## 2. Detection, consent and indicator

- [x] 2.1 Implement the policy gate (enabled, apps, allow_manual, allow_series_auto, require_notice, notice_text, with the spec defaults), with immediate stop and deletion on revocation. Verify with tests that switch the policy mid-capture
- [x] 2.2 Implement meeting detection from audio sessions, microphone use, UI Automation window titles for browsers, and calendar matching. Publish "meeting detected" and "transcript ready" events. Verify the scheduled, ad-hoc and browser scenarios, and that client/scheduler receives both events
- [x] 2.3 Implement the consent Question (Start / Not now) as a standalone Action item in the meeting-origin Thread with 2-minute expiry, the series opt-in and the participant-notice confirmation, with built-in notice texts in cs, de and en. Verify through UI tests
- [x] 2.4 Implement the topmost recording indicator overlay with stop within 2 s and elapsed time. Verify that it stays visible with the main window closed

## 3. Capture and storage

- [ ] 3.1 Implement process-loopback and microphone capture into separate encrypted tracks, plus manual source capture. Verify track separation with a test tone in another app
- [x] 3.2 Implement retention (delete after transcript, keep_audio, max_pending_days sweep). Verify with tests

## 4. Transcription worker

- [ ] 4.1 Implement the stt worker role: VAD, language id, Parakeet ASR, Whisper-small fallback, half the cores at below-normal priority, and the Job memory limit and idle exit from the client/desktop-host stt-worker budget. Verify the 60-minute meeting budget on the baseline laptop (implemented 2026-10-27 on sherpa-onnx 2.0.1; on the development workstation an hour takes 548 s at 1.48 GB peak, design D4; models budget raised to 1.2 GB on 2026-10-31; the baseline laptop run remains)
- [x] 4.2 Implement remote-track diarization, track merge and attendee-based naming, with user rename re-linking knowledge mentions. Verify with fixture meetings
- [x] 4.3 Implement batch-by-default, the live_transcript opt-in, obedience to the client/desktop-host deferral signal, and non-deferred user-started transcription. Verify with tests
- [ ] 4.4 Fuzz the audio container and segment parsers. Verify that the fuzz target runs in CI

## 5. Outputs and integration

- [x] 5.1 Generate summary, decisions and follow-ups via client/model-access. Create the Thread with origin meeting linked to the event, and pass the transcript to client/knowledge-graph when policy.knowledge.sources.meetings is on. Verify end to end with a fixture recording
- [x] 5.2 Attach meeting provenance to transcripts in model contexts, and add the spoken-instruction injection case to the agent eval suite. Verify that no tool call results from it
- [ ] 5.3 Pilot with a consenting team in a pilot Group. Verify the user satisfaction and budget telemetry (metadata only)
- [x] 5.4 Run `openspec validate --all --strict` and fix findings

## Workflow follow-up

- Archive with `openspec archive add-meeting-capture --yes` after review.
- Rollout prerequisites (organizational, not technical gates, analogous to the MS Teams recording policy): before production rollout, the organization-level process completes the DPIA with the data protection officer, the works-council agreement and the legal assessment (GDPR, labour law, external participants), and may supply approved notice texts that admins set as `policy.meeting_capture.notice_text`. Development and limited testing in pilot Groups do not wait for them.
