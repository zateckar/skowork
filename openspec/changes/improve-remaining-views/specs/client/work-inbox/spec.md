# Spec Delta

## ADDED Requirements

### Requirement: Labelled Fields
Every text field in the window SHALL show what it is for: while empty, it SHALL show its label (or a more specific hint) as placeholder text. Fields of a form that the user fills in and reviews as a whole (the Automation editor, the Skill editor, the Daily report time) SHALL also show their label above the field.

#### Scenario: New automation
- **WHEN** the user opens New automation
- **THEN** the name, schedule, prompt and Skill fields each have a visible label, and an empty field shows its label as placeholder text

#### Scenario: Excluded mail folder
- **WHEN** the user looks at Settings > Mail and calendar
- **THEN** the empty field above Exclude folder reads "Folder or category name"

### Requirement: Local Dates And Numbers
The window SHALL convert a moment to local time with the UTC offset in effect at that moment, not today's offset. In every language, times SHALL be shown in 24-hour form ("15:00") and dates as day.month.year ("27.11.2026"); a date in the current year MAY omit the year ("27.11."). A moment today SHALL be shown as its time only. Counts SHALL use the digit grouping of the window's language.

#### Scenario: Summer automation seen in winter
- **WHEN** an Automation runs every Friday at 15:00 in Europe/Prague and the user looks at its next run in September while it is November
- **THEN** the next run reads "25.9.2026 15:00", not 14:00 and not "3:00 PM"

#### Scenario: English window
- **WHEN** the window's language is English and the mailbox last synced on 26 November 2026 at 13:40
- **THEN** Settings shows "26.11.2026 13:40"

#### Scenario: Mail counts
- **WHEN** the local copy holds 18234 messages
- **THEN** Settings shows "18,234 messages" in English

### Requirement: Context Indicator Scope
The status bar SHALL show the context-usage indicator only while a Thread is shown in the window. On the other pages, and with no Thread open, it SHALL NOT show one.

#### Scenario: Settings after a Thread
- **WHEN** the user leaves a Thread for Settings
- **THEN** the status bar shows no context usage

### Requirement: Plain Previews
The preview of an Inbox entry SHALL show the message as text, without Markdown syntax (headings, emphasis, list and quote markers, code fences, link targets).

#### Scenario: Answer with a heading
- **WHEN** the last message of a Thread starts with "## Summary" followed by "The budget for **Q4** is *on track*."
- **THEN** the preview reads "Summary The budget for Q4 is on track."

### Requirement: Readable Approval Arguments
The Approval card SHALL show the arguments of the Tool call as one line per argument, its name and its value; nested values SHALL be shown as compact JSON. Edit arguments SHALL keep the JSON editor.

#### Scenario: Send an email
- **WHEN** an Approval for mail.send has the argument to = "x@evil.example"
- **THEN** the card shows "to" with "x@evil.example", without braces or quotes

### Requirement: Plain Settings Wording
Settings and Status SHALL state each fact once, in words: a folder pattern a user may grant SHALL read as the folder and whether its subfolders are included, with environment variables resolved; a Policy value set by a group SHALL name the group in words and a default SHALL not be marked; Skills SHALL list their Tools by name; a Connector whose token was rejected SHALL offer Replace token; a Connector with no Tools SHALL NOT read "0 of 0 tools usable"; Status SHALL NOT repeat the connection state in its description and SHALL omit a value it does not know; counts SHALL agree in number with their noun; headings SHALL NOT end in an ellipsis; a switch SHALL NOT be followed by a line repeating its state; a managed setting SHALL be marked once. English strings SHALL use US spelling.

#### Scenario: Grantable folders
- **WHEN** the Policy allows granting "%USERPROFILE%\Documents\**"
- **THEN** Settings > Files reads "Documents and its subfolders" with the full path in the tooltip

#### Scenario: Rejected token
- **WHEN** the Wiki Connector reports token-rejected
- **THEN** its actions are Replace token and Remove token

### Requirement: Settings Row Layout
In Settings, the action of a removable item (an excluded folder or category, an indexed folder, a triage folder) SHALL sit at the end of the item's row. Option labels SHALL wrap instead of being cut off. A sub-heading inside a section SHALL have space above it that separates it from the previous group. A captured meeting SHALL show its actions directly under its title and state. The Daily report section SHALL show the configured time in its field and the working days on one wrapping row.

#### Scenario: Long option
- **WHEN** Settings > Triage shows the option to prepare action items automatically
- **THEN** its whole label is visible on as many lines as it needs

### Requirement: App Cards
The Apps page SHALL show each App as a card with its name, a Running badge while it runs, its description, size and last use as a date, Open as a button, and Stop (while running), Show folder, Change with the agent and Delete in a More menu named after the App.

#### Scenario: Running App
- **WHEN** the Notes App runs
- **THEN** its card shows a Running badge next to the name, an Open button and a More menu with Stop, Show folder, Change with the agent and Delete

### Requirement: Tabs Fit Their Labels
A row of tabs SHALL measure each tab with the font it is drawn with, so the chosen tab's label is never cut off while there is room.

#### Scenario: Knowledge view
- **WHEN** the Knowledge view opens on Sources and Documents
- **THEN** the tabs read "Sources" and "Documents 3" in full
