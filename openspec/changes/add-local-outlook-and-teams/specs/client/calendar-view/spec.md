# Spec Delta

## Purpose

Shows the user's calendar inside Skowork, from the local calendar copy of client/mail-calendar.

## ADDED Requirements

### Requirement: Calendar Page
The client SHALL show a Calendar page whenever mail and calendar are available. It offers a day, a work week and an agenda list of the next 14 days. It is built only from the local calendar copy of client/mail-calendar, so it works offline, and it shows the time of the last calendar sync.

#### Scenario: Work week
- **WHEN** the user opens the Calendar page in work-week mode
- **THEN** the events of Monday to Friday of the current week are shown in one column per day in time order, with all-day events first and a marker at the current time in today's column

#### Scenario: Offline
- **WHEN** Outlook or Graph is unreachable
- **THEN** the Calendar page still shows the local copy, marked with the time of the last sync

### Requirement: Calendar Privacy
The Calendar page SHALL show a withheld or private event only as busy time, without its subject, location, body or attendees, as client/mail-calendar masks it for tools.

#### Scenario: Private appointment
- **WHEN** the calendar holds a private appointment and private items are excluded
- **THEN** the Calendar page shows a "Busy" block for its time and nothing else

### Requirement: Calendar Event Actions
The Calendar page SHALL show an event's details and let the user join its online meeting in the default browser, ask Skowork to prepare for the event in a new Thread whose Run gets the event as untrusted content, and open the event in Outlook on the Local Outlook backend. The page SHALL NOT create, change or answer events itself.

#### Scenario: Ask about a meeting
- **WHEN** the user chooses "Ask Skowork" on a meeting
- **THEN** a new Thread opens in which Skowork prepares a brief for the meeting, with the meeting's details given to the Run as untrusted content
