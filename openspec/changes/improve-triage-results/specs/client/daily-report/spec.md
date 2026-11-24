# Spec Delta

## ADDED Requirements

### Requirement: Triage Results In The Daily Report
When event triage is on, the Daily report SHALL include a section "For your information" with the information items client/event-triage kept since the previous report, each with its reason and a Citation of its source. It SHALL also include a section "Action required" with the open triage action items, each linked to its Thread. Both sections SHALL be left out when they are empty.

#### Scenario: Information item reported
- **WHEN** triage kept a release announcement as an information item overnight
- **THEN** the morning's report lists it under "For your information" with its reason
