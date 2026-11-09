# Spec Delta

## MODIFIED Requirements

### Requirement: Local CPU Transcription
Transcription SHALL run entirely on the laptop's CPU in a worker process, with no audio sent to any service. It supports Czech, English and German, with language detected automatically per meeting or set by the user. Other detected or explicitly requested languages are unsupported.

#### Scenario: Czech meeting
- **WHEN** a captured meeting is held in Czech
- **THEN** a Czech transcript is produced locally, and no network request contains audio data

#### Scenario: Unsupported language
- **WHEN** the detected language is not supported
- **THEN** the client reports that transcription is unavailable for that language and keeps the encrypted audio only for the retention period set for pending audio

#### Scenario: Removed language
- **WHEN** Polish or Slovak is explicitly requested for transcription
- **THEN** the worker reports the language is unsupported
