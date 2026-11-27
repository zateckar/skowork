# Spec Delta

## ADDED Requirements

### Requirement: Calendar Time Grid
The day and work-week modes of the Calendar page SHALL show a time grid: one column per day under its date, an hour scale beside the columns, and each timed event as a block placed at its start time whose height is proportional to its length. Events that overlap in time SHALL be placed side by side within their day. All-day events SHALL be shown in a band above the grid. Today's column SHALL be marked and SHALL show a line at the current time. The grid SHALL cover the working hours (08:00 to 18:00), extended to whole hours so that the earliest start and the latest end of the shown events are inside it. Each block SHALL show the start time and the subject as far as its size allows, SHALL keep the privacy of Calendar Privacy, and SHALL open the event's details when chosen. Every block SHALL have a UI Automation name with its day, time and subject. The 14-day agenda list mode stays a list.

#### Scenario: One-hour and half-hour meetings
- **WHEN** Friday holds a meeting from 13:00 to 14:20 and one from 14:00 to 15:00
- **THEN** the work week shows both in Friday's column, the first block about 1.3 times the height of an hour, the second starting at the 14:00 line, and the two side by side because they overlap

#### Scenario: All-day event
- **WHEN** Monday holds an all-day event
- **THEN** it is shown in the band above Monday's column and not in the hour grid
