# Spec Delta

## MODIFIED Requirements

### Requirement: Manual Audio Source Capture
While meeting capture is available and policy.meeting_capture.allow_manual is true (default true), the client SHALL let the user start a capture of a chosen audio source at any time, whether or not a meeting is detected. The source is a specific application, all system output, or a microphone. Starting the capture manually counts as consent, so no "Start capture?" Question is asked. The indicator and participant-notice rules apply as for detected meetings.

#### Scenario: Teams in the browser
- **WHEN** the user is in a Teams meeting in Edge, chooses Edge as the audio source and starts capture
- **THEN** Edge's audio output is captured with the recording indicator shown, and no "Start capture?" Question is asked

#### Scenario: Manual capture turned off by the organisation
- **WHEN** policy.meeting_capture.allow_manual is false
- **THEN** the Meetings settings section offers no audio sources, and a request to start a manual capture is refused

## ADDED Requirements

### Requirement: Manual Capture Tracks
When the user manually captures an application or all system output, the client SHALL also record the user's microphone as the local track, so that the chosen source is the remote track as for a detected meeting. A manual capture of a microphone SHALL record that microphone as its only track.

#### Scenario: The user's own speech
- **WHEN** the user manually captures Chrome during a call and speaks in it
- **THEN** the transcript holds the user's speech labelled as the user, and the other participants' speech from Chrome's track

#### Scenario: Microphone only
- **WHEN** the user manually captures the microphone
- **THEN** one track is recorded, from the microphone

### Requirement: Manual Capture Calendar Match
When a manual capture starts, the client SHALL match it to a calendar event under the same rule as a detected meeting. A matched capture takes the event's subject as its title and is linked to that event, and its attendees are used as for a detected meeting. Without a match, the capture is titled after its source.

#### Scenario: Scheduled meeting in the browser
- **WHEN** the user manually captures Edge within 10 minutes of a calendar event with an online meeting link
- **THEN** the meeting's Thread has the event's subject as its title and is linked to that event

#### Scenario: No calendar event
- **WHEN** the user manually captures Edge and no calendar event matches
- **THEN** the meeting is titled after Edge

### Requirement: Record Meeting From A New Thread
While manual capture is allowed, the composer of a new Thread SHALL have a Record meeting button beside Dictate, with a drop-down that lists the audio sources of Manual Audio Source Capture. When the microphone is in use and exactly one application has active audio output, clicking the button starts a manual capture of that application. Otherwise clicking it opens the list. Once a capture starts, the window opens that meeting's Thread.

#### Scenario: Teams call in the browser
- **WHEN** the user is in a Teams call in Edge, no other application plays audio, and the user clicks Record meeting in the new Thread composer
- **THEN** a manual capture of Edge starts, and the window shows the meeting's Thread

#### Scenario: Several applications play audio
- **WHEN** Edge and a music player both play audio and the user clicks Record meeting
- **THEN** the list of audio sources opens, and nothing is recorded until the user picks one

#### Scenario: Manual capture not allowed
- **WHEN** meeting capture is unavailable or policy.meeting_capture.allow_manual is false
- **THEN** the new Thread composer shows no Record meeting button

### Requirement: Record Meeting Availability
Record meeting and its drop-down SHALL be disabled, with a tooltip that says why, while a capture or a dictation is running.

#### Scenario: A capture is already running
- **WHEN** a capture is running
- **THEN** Record meeting is disabled, and its tooltip says that a meeting is being recorded

### Requirement: Manual Capture Withdraws A Pending Offer
When the user starts a manual capture while a "Start capture?" Question is pending, the client SHALL withdraw that Question, because only one capture runs at a time.

#### Scenario: Detected meeting not answered
- **WHEN** the client has asked "Start capture?" for a detected Teams call and the user instead starts a manual capture of Edge
- **THEN** the Question is withdrawn and the Edge capture runs
