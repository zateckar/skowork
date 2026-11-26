# Spec Delta

## ADDED Requirements

### Requirement: Quality Counters
At the metadata and content levels of policy.telemetry.level, the client SHALL send daily quality counters. They are operational data, like the other metrics listed in Metadata Level Contents. The counters are:
- **Triage:**
  - suggestions created, by category and action;
  - suggestions the user dismissed, marked No action needed, or started work on;
  - suggestions left untouched after 7 days;
  - batch failures, by kind.
- **Agenda:** items admitted, and items marked done, dismissed or edited.
- **Knowledge:** facts stored, and facts the user edited, deleted, or marked more or less important.
- **Meeting capture:**
  - meetings and minutes transcribed;
  - speakers detected;
  - speakers still unnamed 7 days after the meeting;
  - speakers renamed by the user;
  - vocabulary terms the user added;
  - meetings the user had transcribed again;
  - the share of segments marked unclear.

Each counter SHALL carry the client version and the identifier of the instructions and settings in effect (client/evaluation). Counters MUST NOT carry item, Thread, meeting or person identifiers, names, subjects, sender domains, categories created by the user, vocabulary terms, or any other content.

#### Scenario: Counters without content
- **WHEN** at metadata level the user dismisses 6 of 20 triage suggestions in a day
- **THEN** that day's telemetry holds the counts 20 created and 6 dismissed with the triage instruction identifier, and no subject, sender or item identifier

#### Scenario: Telemetry off
- **WHEN** policy.telemetry.level is off
- **THEN** no quality counters are sent
