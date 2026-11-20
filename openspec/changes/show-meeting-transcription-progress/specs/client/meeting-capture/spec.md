# Spec Delta

## ADDED Requirements

### Requirement: Meeting Progress In Its Thread
The meeting's Thread SHALL show a progress line with the meeting's stage: recording with the elapsed time, waiting for transcription with the reason, transcribing with the share done and an estimate of the time left, writing the summary, done, or failed with the reason and Transcribe now. The line changes within 5 seconds of a change of stage, and the share done is updated at least every 10 seconds while the Thread is open.

#### Scenario: Meeting just ended
- **WHEN** the user stops a capture and its transcription starts
- **THEN** the meeting's Thread shows "Transcribing" with the share done and the time left, and the share grows while the user watches

#### Scenario: Waiting on battery saver
- **WHEN** a capture ends while client/desktop-host defers background work
- **THEN** the Thread shows that transcription waits until background work may run, with Transcribe now

#### Scenario: Summary being written
- **WHEN** the transcript is complete and the summary Run is running
- **THEN** the Thread shows that the summary is being written

#### Scenario: Transcription failed
- **WHEN** transcription fails because the speech models are not installed
- **THEN** the Thread shows the failure with its reason

### Requirement: Transcription Progress Estimate
The share done and the time left SHALL be estimated from the length of the meeting audio still to transcribe and the transcription speed measured on this computer. The share stays below 100% until the transcript is complete. Before any speed is measured, the client shows the share without the time left.

#### Scenario: First meeting on a computer
- **WHEN** a meeting is transcribed on a computer that has not transcribed before
- **THEN** the Thread shows the share done and no time left until the first speed is measured

## MODIFIED Requirements

### Requirement: Batch Transcription By Default
Transcription SHALL start after the capture ends, unless policy.meeting_capture.live_transcript is true (default false). With live transcription, the client transcribes the speech captured since the previous pass about every 60 seconds during the capture, and keeps what it has transcribed. After the capture ends, it transcribes only the speech not yet transcribed and separates the speakers once over the whole meeting. Queued transcription is background work and follows the deferral rules of client/desktop-host. A transcription the user starts explicitly is user-initiated and is not deferred.

#### Scenario: Meeting ends while background work is deferred
- **WHEN** a capture ends while client/desktop-host is deferring background work, for example on battery saver
- **THEN** transcription is queued and starts when the deferral ends or when the user starts it

#### Scenario: Live transcription of a long meeting
- **WHEN** live transcription is on and a 20-minute meeting is captured
- **THEN** each audio second is transcribed once, and after the capture ends only the last pass's remainder and the speaker separation run before the summary starts

#### Scenario: Speakers across live passes
- **WHEN** the same remote participant speaks in the first and the last minute of a live-transcribed meeting
- **THEN** the finished transcript gives both segments the same speaker
