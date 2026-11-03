# Spec Delta

## ADDED Requirements

### Requirement: Dictation in Composers
The composers of a Thread, of a new Thread and of a Chat SHALL have a Dictate button that records the default microphone until Stop. Stop transcribes the speech on the device with the speech engine of client/meeting-capture, detecting the language, and inserts the text at the caret. Cancel and Escape discard the recording. Dictation never sends anything, and a recording stops by itself after 5 minutes.

#### Scenario: Dictate a request
- **WHEN** the user clicks Dictate in a Thread's composer, says "Summarise my unread e-mail from today" and clicks Stop
- **THEN** that text appears in the composer within a few seconds, and no message is sent until the user sends it

#### Scenario: Discard
- **WHEN** the user presses Escape while recording
- **THEN** the recording stops, nothing is inserted and the audio is deleted

#### Scenario: Czech
- **WHEN** the user dictates in Czech
- **THEN** the inserted text is Czech, with punctuation and capitals

### Requirement: Dictation Language
Settings, General SHALL have a Dictation language choice: Automatic (the default) or one of the transcript languages of client/meeting-capture (Czech, English, German, Polish, Slovak). A chosen language is used without detection. Automatic detects the language; when the detected language is not one of them, dictation uses the language of the window instead of failing.

#### Scenario: Czech chosen
- **WHEN** the user chooses Čeština as the dictation language and dictates in Czech
- **THEN** the Czech text is inserted, whatever language detection would have said

#### Scenario: Short speech misdetected
- **WHEN** the dictation language is Automatic, the window is in Czech, and detection takes a short Czech dictation for a language that is not supported
- **THEN** the dictation is transcribed as Czech instead of failing

### Requirement: Dictation Privacy
Dictation SHALL record only after the user clicks Dictate, and only until Stop, Cancel, Escape or the 5-minute limit. The audio MUST stay on the device: it is written encrypted to a temporary file under the data root and deleted when it is transcribed or discarded, and at the next start if the client ended while recording. Neither the audio nor the transcript is sent anywhere by Dictation itself.

#### Scenario: Client closed while recording
- **WHEN** the client ends while a dictation is recording
- **THEN** the next start deletes the temporary audio

### Requirement: Dictation Availability
The Dictate button SHALL be shown while policy.dictation.mode is on. It is disabled, with a tooltip that says why, while the speech models are not installed, no microphone is present, or a meeting capture is running.

#### Scenario: No speech models
- **WHEN** the client has no speech models installed
- **THEN** the Dictate button is disabled and its tooltip says that speech recognition is not installed
