# Spec Delta

## ADDED Requirements

### Requirement: Continuation Blocker Codes
When an interrupted Run cannot be continued, the runtime SHALL report why as one of the codes missing, subagent, not_interrupted, stopped, already_continued, newer_run, active_run or unknown_outcome. The window SHALL show the code's translated text (client/work-inbox), never runtime wording.

#### Scenario: A newer Run followed
- **WHEN** the user opens a Thread whose interrupted Run was followed by a newer Run
- **THEN** the recovery status reports blocker_code "newer_run" and the banner shows the translated reason
