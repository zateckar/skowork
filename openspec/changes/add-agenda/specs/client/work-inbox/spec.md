# Spec Delta

## ADDED Requirements

### Requirement: Agenda View
When the Agenda is enabled (client/agenda), the Inbox views SHALL include an Agenda view listing open Agenda items grouped as Overdue, Today, Tomorrow, This week and Later, sorted by due time. Each row SHALL show the title, the due date with an inferred marking where applicable, the kinds of its sources and whether a preparation exists, and SHALL offer Prepare or Open preparation, Open source, Mark done, Dismiss and Edit date. Every row and action SHALL have a UI Automation name.

#### Scenario: Open the Agenda
- **WHEN** the user has two open items due today and one due next week
- **THEN** the Agenda view shows them under Today and Later, and each row offers Prepare

#### Scenario: Agenda disabled
- **WHEN** the Agenda is disabled
- **THEN** the Agenda view is not listed among the Inbox views
