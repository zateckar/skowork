# Spec Delta

## ADDED Requirements

### Requirement: Remote Audio From The Output Device
While the remote track of a meeting records the meeting application by process loopback, the client SHALL measure, without recording it, the loopback of the output device on which a process of the application's tree has an active audio session. When that device has carried 3 seconds of sound while process loopback has delivered only digital silence since the capture started, the client SHALL record the remote track from the loopback of that device for the rest of the capture, and follow the application when it plays to another output device. Once process loopback delivers sound, the client SHALL stop measuring the device and keep process loopback.

#### Scenario: Teams call audio missing from process loopback
- **WHEN** a Teams meeting is captured, process loopback of the Teams process tree delivers only digital silence, and the headset Teams plays to carries the other participants' voices
- **THEN** within a few seconds of the first remote speech the remote track records the headset's output, and the transcript contains the other participants' speech

#### Scenario: Process loopback works
- **WHEN** a meeting application's process loopback delivers sound
- **THEN** the remote track stays on process loopback, and no other sound on the output device is recorded

#### Scenario: Quiet start
- **WHEN** nobody speaks for the first minutes of a meeting
- **THEN** the remote track stays on process loopback until the output device carries sound
