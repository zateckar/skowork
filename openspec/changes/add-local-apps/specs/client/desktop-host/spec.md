# Spec Delta

## MODIFIED Requirements

### Requirement: Footprint Budgets
On the baseline hardware, the client SHALL stay within these budgets:
- the core package installs in 30 MB or less, including its storage, search and document libraries;
- the optional speech-to-text package installs in 40 MB or less of runtime plus at most 1.2 GB of models;
- the optional app runtime package installs in 100 MB or less;
- the core process uses a working set of 60 MB or less when idle with the window closed;
- average CPU is 0.5% or less when idle;
- the tray is ready 2 seconds or less after process start.

#### Scenario: Idle budget check
- **WHEN** the client has been idle for 10 minutes with the window closed and no background job running
- **THEN** the core process working set is at most 60 MB and its average CPU over the period is at most 0.5%

#### Scenario: Models budget covers all local models
- **WHEN** the optional speech-to-text package is measured
- **THEN** the speech-to-text models with their fallback, and the voice-activity and diarization models, together take at most 1.2 GB, and the package contains no other model
