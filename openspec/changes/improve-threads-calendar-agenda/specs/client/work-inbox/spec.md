# Spec Delta

## ADDED Requirements

### Requirement: In-Window Links
A link in model output whose target is a place in the window (a Thread or another route of the window) SHALL open that place in the window when the user clicks it, without the browser confirmation of Markdown Rendering. A link to a Thread that no longer exists SHALL open the Inbox. Links to web addresses keep the confirmation, and other targets stay refused.

#### Scenario: Daily report link to a Thread
- **WHEN** the user clicks the "Thread" link of an entry under "Action required" in a Daily report
- **THEN** that Thread opens in the reading pane, and no confirmation is shown

#### Scenario: Web link
- **WHEN** the user clicks a link to an https address in an answer
- **THEN** the target URL is shown for confirmation before it opens in the default browser

### Requirement: Thread Header
The Thread header SHALL state each fact once: the title, then one line with the origin badge and only what the badge and the title do not already say. A Thread the client created itself (System Threads) SHALL NOT repeat its badge in words. A meeting Thread SHALL show the date and time the meeting took place, its length, and its attendees (the first few by name and how many more), and SHALL link to the meeting's calendar event when one is known; it SHALL NOT offer a link that opens Settings.

#### Scenario: Meeting Thread
- **WHEN** the user opens the Thread of a captured meeting that was matched to a calendar event with six attendees
- **THEN** the header shows the title once, the "Meeting" badge, the meeting's date, start and end time, the attendees, and a link that opens the calendar event, and nothing that opens Settings

#### Scenario: Daily report
- **WHEN** the user opens a Daily report Thread
- **THEN** the header shows the title and the "Skowork" badge, and not also "Created by Skowork"

### Requirement: Live List Entries
A change to a Thread's list entry (title, badges, unread, archived, origin) SHALL reach the Inbox views as it happens, whichever part of the client made it, without the user reloading the list. When the user decides "No action needed" or dismisses the suggestion of a triage Thread, its "Needs your action" badge SHALL go away.

#### Scenario: No action needed
- **WHEN** the user chooses "No action needed" in a triage Thread that shows "Needs your action"
- **THEN** within a second the list entry no longer shows "Needs your action", and the Thread leaves the Inbox view because it is archived

#### Scenario: Triage creates a Thread
- **WHEN** triage creates a Thread while the Inbox view is open
- **THEN** the entry appears in the list without the user reloading it

### Requirement: Agenda View Layout
The Agenda view (Agenda View) SHALL show each Agenda item as a separate card, with the groups as headings that show how many items they hold. A card SHALL show, in this order: the title; the due date, marked overdue or inferred where it applies; the action in one or two lines; the kinds of its sources and whether a preparation exists as badges; a quote from its source shortened to one line; and its actions. Prepare or Open preparation SHALL be the one primary button, Open source and Mark done SHALL be buttons beside it, and Dismiss, Edit date and Rename SHALL be in a menu of the card. Every card, button and menu item SHALL have a UI Automation name that includes the item's title.

#### Scenario: Overdue items
- **WHEN** the Agenda holds three overdue items and one due today
- **THEN** the view shows the heading "Overdue" with 3 and three cards, then "Today" with 1 and one card, and each overdue card marks its due date as overdue

#### Scenario: Less frequent actions
- **WHEN** the user opens the menu of a card
- **THEN** it offers Dismiss, Edit date and Rename for that item

### Requirement: Settings Navigation
The Settings page SHALL show its sections (Settings Structure) as a vertical list beside the content, grouped under the headings Personal, Sources, Assistant and Organization, and SHALL show only the chosen section's content. The chosen section SHALL be marked in the list and exposed as selected to UI Automation. Each section SHALL have its own route (#/settings/<section>), so that other places of the window open the section they refer to; #/settings opens the section chosen last, or General.

#### Scenario: Choose a section
- **WHEN** the user chooses "Meetings" in the Settings list
- **THEN** only the Meetings section is shown, and "Meetings" is marked as the chosen section

#### Scenario: Link to a section
- **WHEN** the user chooses "Open settings" on the Calendar page while mail is not set up
- **THEN** Settings opens on the Mail and calendar section

### Requirement: Plain Information
The window SHALL show the user what they need for the task in front of them, in their language, and no more:
- times SHALL be shown in local time in the window's language, never as raw timestamps or in UTC;
- internal identifiers, protocol counters and token lifetimes SHALL NOT be shown in the Status, Settings or Thread views;
- names of internal categories (such as storage categories) SHALL be shown as translated labels;
- a valid Policy bundle SHALL NOT be mentioned in the status bar; an expired or missing one SHALL be;
- an error banner SHALL offer the remedy that can work as its primary action, and SHALL NOT offer Retry where retrying cannot succeed (a Thread too long for the model);
- a choice among several values of one action (such as pause durations) SHALL be one control with a menu, not one button per value;
- removing data that cannot be restored (such as forgetting a captured meeting) SHALL ask for confirmation first.

#### Scenario: Last sync of the mailbox
- **WHEN** the mailbox last synced today at 13:40 local time
- **THEN** Settings shows the time as 13:40 in the window's format, without a date in ISO form or "UTC"

#### Scenario: Thread too long
- **WHEN** a Run fails because the Thread is too long for the model
- **THEN** the banner offers Compact thread as its primary action and Fork from here, and no Retry
