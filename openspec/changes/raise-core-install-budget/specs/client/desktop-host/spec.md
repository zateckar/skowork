# Spec Delta

## MODIFIED Requirements

### Requirement: Footprint Budgets
On the baseline hardware, the client SHALL stay within these budgets:
- core package, with its storage, search and document libraries: 40 MB or less installed;
- optional speech-to-text package: 40 MB or less of runtime plus at most 1.2 GB of models;
- optional app runtime package: 100 MB or less installed;
- core process working set when idle with the window closed: 60 MB or less;
- average CPU when idle: 0.5% or less;
- tray ready: 2 seconds or less after process start.

#### Scenario: Idle budget check
- **WHEN** the client has been idle for 10 minutes with the window closed and no background job running
- **THEN** the core process working set is at most 60 MB and its average CPU over the period is at most 0.5%

#### Scenario: Models budget covers all local models
- **WHEN** the optional speech-to-text package is measured
- **THEN** the speech-to-text models with their fallback, and the voice-activity and diarization models, together take at most 1.2 GB, and the package contains no other model

#### Scenario: Core works without speech-to-text package
- **WHEN** the optional speech-to-text package is not installed
- **THEN** the client runs, and only meeting transcription reports that it is unavailable

#### Scenario: Core package size check
- **WHEN** the release build's core package is measured
- **THEN** Skowork.exe, skowork-worker.exe and pdfium.dll together take at most 40 MB
