# Spec Delta

## MODIFIED Requirements

### Requirement: Dictation Language
Settings, General SHALL offer Automatic (the default), Czech, English and German. A chosen language is used without detection. Automatic detects the language and uses the window language when detection is unsupported. A previously stored language outside these choices is treated as Automatic, so existing settings do not prevent dictation.

#### Scenario: Czech chosen
- **WHEN** the user chooses Čeština as the dictation language and dictates in Czech
- **THEN** the Czech text is inserted, whatever language detection would have said

#### Scenario: Short speech misdetected
- **WHEN** the dictation language is Automatic, the window is in Czech, and detection takes a short Czech dictation for a language that is not supported
- **THEN** the dictation is transcribed as Czech instead of failing

#### Scenario: Previous language is no longer supported
- **WHEN** an existing profile has Polish or Slovak selected for dictation
- **THEN** the client shows and uses Automatic until the user selects another supported language

### Requirement: Dictation Availability
The Dictate button SHALL be shown while policy.dictation.mode is on. Missing, corrupt or incompatible speech packages show a Set up speech recognition action under client/speech-models instead of a disabled dead end. No microphone, microphone access denied, device loss and an active meeting capture each show a distinct reason and appropriate recovery action. These actions never start recording implicitly.

#### Scenario: No speech models
- **WHEN** the client has no speech models installed
- **THEN** the composer offers Set up speech recognition, which opens client/speech-models setup and preserves the draft

#### Scenario: Microphone access denied
- **WHEN** Windows refuses microphone access
- **THEN** the composer explains the permission problem and offers to open the relevant Windows settings, without recommending a model download

#### Scenario: Meeting owns the microphone
- **WHEN** a meeting capture is running
- **THEN** Dictate is unavailable with a meeting-capture explanation and becomes available after capture ends if all other checks pass

## ADDED Requirements

### Requirement: Dictation Device Failure
If the microphone disappears or capture fails during Dictation, the client SHALL stop the recording, retain the existing composer draft, report the device error and discard the incomplete audio. A subsequent Dictate attempt SHALL recheck current device availability.

#### Scenario: Unplug a microphone
- **WHEN** the microphone is disconnected while the user dictates
- **THEN** recording stops, the draft is unchanged, the error identifies the device problem, and reconnecting allows a fresh attempt without restarting the app
