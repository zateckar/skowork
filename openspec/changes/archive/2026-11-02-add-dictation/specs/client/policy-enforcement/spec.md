# Spec Delta

## ADDED Requirements

### Requirement: Dictation Policy
The client SHALL apply policy.dictation.mode (off or on, default on, owned by client/dictation). While it is off, the composers show no Dictate button and the dictation RPCs refuse with policy_denied. Switching it off during a recording discards the recording.

#### Scenario: Switched off
- **WHEN** an administrator sets policy.dictation.mode to off for the user's Group
- **THEN** the Dictate button disappears from the composers at the next policy update
