# Spec Delta

## MODIFIED Requirements

### Requirement: Occurrence Lifecycle
Each Trigger firing and accepted Remote task SHALL create an Occurrence with pending, running and terminal states. An interrupted Run or an unknown effect awaiting user choice keeps it running. Other failed Occurrences are retried up to 3 attempts with exponential backoff, then become dead. An Occurrence with an unknown effect MUST NOT retry automatically, including after a continuation fails. User or issuer cancellation cancels it.

#### Scenario: User stops an Occurrence's Run
- **WHEN** the user presses Stop on a Run started by an Occurrence
- **THEN** the Occurrence becomes cancelled, is not retried, and the history shows "stopped by user"

#### Scenario: Restart while waiting for approval
- **WHEN** the client restarts while an Occurrence's Run is interrupted for an Approval
- **THEN** the Occurrence is still running with the same Run and Approval, and no new attempt is started

#### Scenario: Transient failure
- **WHEN** an Occurrence's Run fails with MODEL_UNAVAILABLE on the first attempt and has no unknown effect
- **THEN** it is retried after backoff, and the history shows each attempt with its error

#### Scenario: Dead occurrence
- **WHEN** all 3 attempts fail without an unknown effect
- **THEN** the Occurrence is marked dead and, if notifications are enabled, the user is notified

#### Scenario: Unknown effect after shutdown
- **WHEN** an Occurrence's failed Run has an unknown effect
- **THEN** the Occurrence releases its slot, remains linked to that Run, and is not automatically retried while it awaits user resolution

#### Scenario: Resolve held occurrence
- **WHEN** the user continues a held Occurrence and its continuation ends
- **THEN** the Occurrence records the continuation's final outcome once without creating another automatic attempt for the unknown effect
