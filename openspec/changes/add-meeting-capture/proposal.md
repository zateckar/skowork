# Proposal

## Why

Many decisions and follow-ups are agreed in calls (Teams, Zoom, Jitsi and others) and are never written down. Capturing meeting audio locally with the user's consent, and transcribing it on the laptop's CPU, turns those calls into searchable transcripts, summaries and follow-ups. No audio has to leave the device.

## What Changes

- Add meeting detection. Audio activity in known meeting apps (browsers by window title) is cross-checked with the calendar, and an explicit consent Question follows. Per-series auto-start is optional. "Meeting detected" and "transcript ready" events are published for the Trigger kind meeting.
- Add capture with a persistent recording indicator and one-click stop. Remote participants (process loopback) and the local user (microphone) are recorded on separate tracks. Manual "any audio source" capture is also added.
- Add local CPU speech-to-text with voice activity detection and diarization of the remote track. Speakers are named from calendar attendees, and the user can correct names.
- Add outputs. The transcript, summary, decisions and follow-ups go into a Thread with meeting origin, and into client/knowledge-graph (Source meetings) when that Source is on.
- Add privacy controls:
  - disabled by default and enabled per Group by policy (regions are expressed as Groups);
  - raw audio encrypted while pending and deleted after transcription by default;
  - a copyable participant-notice text.
- Add resource budgets fitting the baseline laptop: i5, 16 GB RAM, iGPU, CPU-only.

**Enablement:** the feature is controlled only by `policy.meeting_capture.enabled` per Group (default false). Production rollout requires organization-level sign-off (data protection officer, works council), analogous to the existing MS Teams recording policy. This is organizational and not enforced technically. Development and limited testing in pilot Groups are allowed without it.

Out of scope:
- Joining meetings as a bot.
- Uploading audio to any server. Transcripts leave the device only as LLM context, sent through client/model-access.
- Real-time translation.
- Video capture.

## Capabilities

### New Capabilities
- `client/meeting-capture`: meeting detection, consent, audio capture, local transcription and diarization, meeting outputs, retention and resource budgets

### Modified Capabilities
- None.

## Impact

- **Roadmap:** change 13 of 13 (Knowledge). Production rollout follows the organization-level sign-off described under What Changes; implementation does not wait for it.
- **Depends on:**
  - add-client-foundation (worker processes and the stt-worker and models budgets of client/desktop-host, storage)
  - add-agent-runtime (summary generation via model-access)
  - add-policy-and-sandbox (policy.meeting_capture)
  - add-work-inbox (standalone Action item for the consent Question in a meeting-origin Thread)
  - add-mail-calendar (attendees, calendar events)
  - add-scheduled-and-remote-tasks (Trigger kind meeting, which consumes this change's events)
  - add-knowledge-graph (Source meetings)
- **Forward references:** none. This is the last change in the roadmap.
- **New code:**
  - the audio capture module, using WASAPI process loopback and the microphone;
  - the stt role of `workbench-worker.exe`, built on sherpa-onnx with the Parakeet-TDT model, Silero VAD and diarization.
- **Optional artifacts:** the Parakeet speech model (about 650 MB on disk) and the Whisper-small fallback (about 357 MB as int8 ONNX). Both are part of the models package, count toward the client/desktop-host models budget, and are delivered by the distribution system only to Groups where the feature is enabled.
- **Licensing:** attribution under CC-BY-4.0 for the speech model.
- **Governance (organizational, not technical):** before production rollout, the organization-level process covers the GDPR and Czech labour-law assessment, the works-council agreement and a DPIA, with the MS Teams recording policy as precedent.
