# Spec Delta

## MODIFIED Requirements

### Requirement: Generated Thread Surfaces
The Thread feed SHALL render supported, complete generated surfaces embedded in assistant messages, alongside their text, using native components. Surfaces SHALL support composable sections, cards, read-only text, rich text with links and Citations, key facts, status callouts, timelines, task lists with owner, due date, state and in-window link, message drafts with a local Copy action, tables, bar and line charts, and directed relationship diagrams.

#### Scenario: Visual comparison
- **WHEN** an assistant message contains a complete supported surface with a table and a bar chart
- **THEN** the Thread shows both visualizations at that point in the answer with the surrounding text

#### Scenario: Non-assistant content
- **WHEN** a user message, Chat message or tool result contains surface syntax
- **THEN** the feed treats it as content and does not activate a surface

#### Scenario: Copy a draft
- **WHEN** the user presses Copy on a message draft in a surface
- **THEN** the draft's body is on the clipboard and nothing is sent

### Requirement: Generated Surface Setting
The client SHALL provide a disabled-by-default setting for generated surfaces. Changes SHALL apply on the next client restart, and the setting SHALL explain this to the user. When disabled, the client SHALL retain the original readable response, SHALL NOT activate generated controls and SHALL NOT request generated surfaces in chat answers. Surfaces without controls SHALL render whether or not the setting is enabled, and Content templates SHALL be requested whether or not it is enabled.

#### Scenario: Enable generated surfaces
- **WHEN** the user enables generated surfaces in Settings and restarts the client
- **THEN** eligible responses can display generated surfaces with controls and the runtime can request them

#### Scenario: Disable generated surfaces
- **WHEN** the user disables generated surfaces and restarts the client
- **THEN** existing responses remain readable, their generated controls are inactive, and surfaces without controls still render

## ADDED Requirements

### Requirement: Content Templates
A generated surface MAY name a Content template and a title. The Thread SHALL show such a surface with a header that has the template's icon and its name in the user's interface language, followed by the title. The Content templates are daily_report, meeting_summary, briefing, action_prep and meeting_prep.

#### Scenario: Daily report header
- **WHEN** the Daily report answer contains a valid surface with template daily_report and title "Friday 27 November"
- **THEN** the Thread shows "Daily report" with its icon and the title above the report's components

### Requirement: Surface Text In Previews
Thread list previews, notifications and other text excerpts of an assistant message SHALL show a valid generated surface as its summary and SHALL omit an invalid one, never its JSON.

#### Scenario: Preview of a Daily report
- **WHEN** the latest message of a Thread is a Daily report with a template surface
- **THEN** the Thread list preview shows the report's prose and the surface summary, not JSON
