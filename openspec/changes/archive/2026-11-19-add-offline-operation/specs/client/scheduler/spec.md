# Spec Delta

## ADDED Requirements

### Requirement: Occurrences While Disconnected
An Occurrence whose Run cannot start because the client is disconnected (client/control-channel Offline Operation) SHALL stay pending without counting an attempt, and its Run SHALL start when the client is connected again. A schedule Trigger keeps at most one such pending Occurrence; further firings while it waits are recorded as skipped.

#### Scenario: Daily Trigger during a day offline
- **WHEN** a daily 08:00 Trigger fires while the client is disconnected, and the client connects again at 17:00
- **THEN** the Occurrence is still pending with no failed attempt until 17:00, and its Run starts after the client connects
