# Spec Delta

## ADDED Requirements

### Requirement: UI Thread Work Limit
Each task the desktop UI runs SHALL finish within 50 milliseconds on the baseline laptop, also while a Run streams and background work runs. A task that needs longer SHALL run in the background, and the UI SHALL show its result when it is ready.

#### Scenario: Streamed Run under load
- **WHEN** a scripted Run streams 2,000 text deltas over 20 seconds while four background requests are pending
- **THEN** no UI task takes longer than 50 milliseconds, the 95th percentile of UI tasks is 16 milliseconds or less, and a click is answered within 100 milliseconds

### Requirement: Background Work Off The UI Thread
The desktop UI SHALL NOT open a database, run scheduler work or scan a folder. Periodic work runs in the background, and at most one run of each kind is in progress at a time. UI requests SHALL NOT wait behind background work.

#### Scenario: Slow periodic work
- **WHEN** a scheduler tick in the background takes 30 seconds
- **THEN** the UI keeps answering its requests, and the next tick does not start until that tick has ended
