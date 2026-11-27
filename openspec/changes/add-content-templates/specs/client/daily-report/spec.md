# Spec Delta

## ADDED Requirements

### Requirement: Daily Report Presentation
The Daily report SHALL be presented with the daily_report Content template (client/work-inbox Content Templates): today's events as a timeline, what the user should know grouped by topic with Citations, and Agenda and triage items as task lists with due dates and links to their Threads, each section only when it has content.

#### Scenario: Morning report
- **WHEN** the Daily report runs on a day with two meetings and one overdue Agenda item
- **THEN** the Thread shows a timeline with the two meetings and a task list with the overdue item marked overdue
