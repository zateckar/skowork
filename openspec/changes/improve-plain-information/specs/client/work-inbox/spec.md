# Spec Delta

## ADDED Requirements

### Requirement: Plain Action Details
The Approval card SHALL name the Tool in words where the client knows a name for it, and otherwise by its Connector and operation. Each untrusted source SHALL be named by its kind and a short, readable reference: a file by its file name, a web page by its host, other sources by their kind alone. The full reference SHALL be available as the item's tooltip and UI Automation help.

#### Scenario: Approval after reading a mail
- **WHEN** an Approval for mail.send is raised after the Run read a mail message
- **THEN** the card is titled "Approval required: Send an email" and lists the source as "Email" without the message's internal id

### Requirement: Plain Settings Details
Settings sections SHALL show what the user decides by and put technical details behind a Details toggle. Skills SHALL show each Skill's name, tier, state, description, translated risk and Tools, with its source, version and other copies behind Details. Policy SHALL show when the Policy bundle expires instead of its version, and SHALL show decisions (allow, approve, deny) and risk classes as translated words. Automations SHALL show times in local time in the window's format and SHALL describe a daily, workday, weekly or hourly schedule in words; other schedules read "Custom schedule" with the expression in the tooltip. An item with several actions (a Knowledge source, an Automation) SHALL keep its on/off switch and at most one other visible action, with the rest in a More menu named after the item. Skipped Knowledge items SHALL be shown by file name with a translated reason. Items of a list in Settings SHALL be separated by one spacing: 16 DIP between items of several lines, 8 DIP between one-line items. The switch of an item SHALL say on or off, so its heading SHALL NOT repeat that state.

#### Scenario: Workday automation
- **WHEN** an Automation runs on the cron schedule "0 8 * * 1-5"
- **THEN** its row reads "Workdays at 08:00", shows the next time in local time, and offers Run now and Edit in its More menu

#### Scenario: Knowledge source
- **WHEN** the user looks at the Mail Knowledge source
- **THEN** it has an on/off switch and a More menu with Pause, Reindex and Forget

### Requirement: Selected Tabs
Every row of tabs in the window (the Knowledge view's sections and Source tabs) SHALL mark the chosen tab visibly and expose it as selected to UI Automation.

#### Scenario: Knowledge sections
- **WHEN** the user opens the Review section of the Knowledge view
- **THEN** the Review tab is drawn as chosen and UI Automation reports it as selected

### Requirement: Compact Chat Header
A team Chat's header SHALL show the Chat's name, the number of members (with their names and presence in the tooltip), Add people as a button, and Rename, Save as note and Leave in a More menu.

#### Scenario: Team Chat with five members
- **WHEN** the user opens a team Chat with five members
- **THEN** the header reads "5 members" and shows Add people and a More menu
