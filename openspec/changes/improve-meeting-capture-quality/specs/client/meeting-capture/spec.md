# Spec Delta

## ADDED Requirements

### Requirement: Echo Removal From The Local Track
The client SHALL leave out of the transcript every local-track segment that repeats speech of the remote track heard through the loudspeakers at the same moment. A segment is left out only when both its words and its audio match the remote track. Segments left out do not appear in the meeting outputs and are not passed to client/knowledge-graph.

#### Scenario: Meeting without a headset
- **WHEN** a participant's sentence is played by the laptop loudspeakers and picked up by the microphone
- **THEN** the transcript shows that sentence once, attributed to the remote speaker, and no segment attributed to the user repeats it

#### Scenario: The user's own short reply
- **WHEN** the user says "yes, I can do that" while no remote speech is reaching the microphone
- **THEN** the segment stays in the transcript, attributed to the user

#### Scenario: Read-back
- **WHEN** the user repeats a participant's sentence two seconds after the participant finished it
- **THEN** both segments stay in the transcript

#### Scenario: Headset in use
- **WHEN** the user wears a headset for the whole meeting, so no remote speech reaches the microphone
- **THEN** no local-track segment is left out

### Requirement: Echo Review
The Meetings settings section SHALL show, for each transcribed meeting, how many segments were left out as echo, and let the user view them and restore any of them. A restored segment returns to the transcript at its time, attributed to the user, and the knowledge derived from the meeting is updated as it is for a speaker rename.

#### Scenario: Restore a segment
- **WHEN** the user restores a segment that was left out as echo
- **THEN** the segment appears in the transcript at its original time, and client/knowledge-graph receives the updated transcript when policy.knowledge.sources.meetings is on

#### Scenario: Nothing left out
- **WHEN** a meeting has no segment left out as echo
- **THEN** the section shows no echo entry for that meeting

### Requirement: Meeting Vocabulary
The client SHALL give transcription a vocabulary for each meeting, made of the user's own name, the names of the matched calendar event's attendees, the organisation names in those attendees' email addresses other than public mail providers, and the user's vocabulary terms. The vocabulary is used for transcription only: it MUST NOT enter a model context other than as words of the transcript.

#### Scenario: Meeting without a calendar event
- **WHEN** an unscheduled meeting is transcribed
- **THEN** the vocabulary holds only the user's own name and the user's vocabulary terms

#### Scenario: Public mail provider
- **WHEN** an attendee's address belongs to a public mail provider
- **THEN** the provider's name is not added to the vocabulary

#### Scenario: Name absent from the transcript
- **WHEN** an attendee's name is in the vocabulary and nobody says it
- **THEN** the model context that produces the summary, decisions and follow-ups does not contain that name

### Requirement: Vocabulary Spelling
Transcription SHALL favour the terms of the meeting vocabulary. A recognised word that differs from a vocabulary term by one letter, either whole or without its inflectional ending, takes that term's spelling and keeps its ending. A difference in the last letter alone is an ending, not a misspelling.

#### Scenario: Attendee name
- **WHEN** the matched calendar event lists the attendee "Zatecka" and a participant says that name
- **THEN** the transcript spells the name "Zatecka"

#### Scenario: Inflected name
- **WHEN** the vocabulary holds "Oldřich" and a participant says "knížete Oldřicha"
- **THEN** the transcript spells the name "Oldřicha"

#### Scenario: Unrelated word
- **WHEN** a recognised word differs from every vocabulary term by more than one letter
- **THEN** the word stays as recognised

#### Scenario: Different ending
- **WHEN** the vocabulary holds "Svobodová" and the transcript has "paní Svobodové"
- **THEN** the word stays as recognised, because only its ending differs

### Requirement: User Vocabulary Terms
The Meetings settings section SHALL let the user keep up to 200 vocabulary terms, with add and remove. A change applies to meetings transcribed after it.

#### Scenario: Term added later
- **WHEN** the user adds a term in the Meetings settings section
- **THEN** the term applies to meetings transcribed from then on, and transcripts already produced are unchanged

#### Scenario: Limit reached
- **WHEN** the user has 200 terms and adds another
- **THEN** the section refuses the term and says the limit is reached

### Requirement: Capture Across Device Changes
A running capture SHALL continue when the default microphone or output device changes, or when an audio stream fails. The client reopens the affected stream on the current default device within 5 seconds, and the time without audio is recorded as silence so that the tracks stay aligned.

#### Scenario: Headset plugged in
- **WHEN** the user plugs in a headset during a capture and Windows makes its microphone the default
- **THEN** the local track continues from the headset microphone within 5 seconds, and the speech after the switch has the same meeting time on both tracks

#### Scenario: Stream cannot be reopened
- **WHEN** the only microphone is unplugged during a capture and none becomes available within 30 seconds
- **THEN** the capture continues with the remote track, and the client keeps trying to reopen the microphone

### Requirement: Capture Trouble Indication
While a track of a running capture has no working audio stream, the recording indicator SHALL show which track is not being recorded. The indication disappears when the stream works again.

#### Scenario: Microphone removed
- **WHEN** the microphone is unplugged during a capture
- **THEN** the recording indicator shows that the microphone is not being recorded

#### Scenario: Stream recovers
- **WHEN** a failed stream is reopened
- **THEN** the trouble indication disappears from the recording indicator

### Requirement: Captured Audio Survives A Client Stop
Audio of a running capture SHALL be stored at least once per second, so that a client that stops unexpectedly loses at most the last second. After the client starts again, the audio captured before the stop is kept under the Audio Retention rules and transcribed unless the capture is continued.

#### Scenario: Client stops during a meeting
- **WHEN** the client process ends unexpectedly 20 minutes into a capture and the meeting ends before the client is started again
- **THEN** the first 20 minutes, less at most one second, are transcribed

#### Scenario: Policy disabled meanwhile
- **WHEN** policy.meeting_capture.enabled is false after the restart
- **THEN** the audio captured before the restart is deleted as for a policy revoked during a meeting

### Requirement: Continue Capture After A Restart
When the client starts again and the interrupted meeting is still detected within 10 minutes of the interruption, the client SHALL ask "Continue capture?" with Continue and Finish, as a standalone Action item in the meeting's Thread presented by client/work-inbox. Continue resumes the capture in the same meeting, with the interruption recorded as silence and without repeating the participant notice.

#### Scenario: Continue after a restart
- **WHEN** the client is started again while the meeting still runs and the user chooses Continue
- **THEN** capture resumes with the recording indicator shown, and the transcript is one transcript of one meeting with the later speech at its real meeting time

#### Scenario: Finish
- **WHEN** the user chooses Finish
- **THEN** no audio is captured after the restart, and the audio captured before it is queued for transcription

#### Scenario: Question ignored
- **WHEN** the user does not answer "Continue capture?" within 2 minutes
- **THEN** the Question is withdrawn, no audio is captured after the restart, and the audio captured before it is queued for transcription

#### Scenario: Meeting no longer detected
- **WHEN** the interrupted meeting is not detected within 10 minutes of the interruption
- **THEN** no Question is raised, and the audio captured before the restart is queued for transcription
