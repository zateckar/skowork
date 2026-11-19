# Spec Delta

## Purpose

Defines the email-like inbox through which users start, follow, triage and unblock agent work: Threads, smart views, Action item cards, the composer, notifications, search and the overall UI structure.

## ADDED Requirements

### Requirement: Thread List Entries
The inbox SHALL show each Thread as a list entry with:
- an automatically generated title that the user can rename;
- a preview of the last message;
- the relative time of the last activity;
- an unread indicator;
- an Action indicator when the Thread has an Action item;
- a running indicator when a Run is active;
- an origin badge: user, trigger, remote, meeting, fork or system (System Threads).

#### Scenario: New Thread gets a title
- **WHEN** the first Run of a new Thread produces its first response
- **THEN** the Thread's list entry shows a generated title of at most 80 characters, and the user can rename it

#### Scenario: Origin badge
- **WHEN** a Thread was created by a Trigger
- **THEN** its list entry shows the trigger origin badge

### Requirement: System Threads
The client SHALL give the system origin to the Threads it creates itself: a Thread that holds a standalone Action item, such as Connect mailbox, and a Thread that runs a Skill test (client/skills Skill Test Run). The badge of a Skill test Thread SHALL read Skill test, and its origin link SHALL open the Skill in the editor.

#### Scenario: System Thread
- **WHEN** the client needs the user to connect their mailbox
- **THEN** a Thread with the system origin badge holds the Connect mailbox Action item

#### Scenario: Skill test Thread
- **WHEN** the user runs a test of a Skill draft
- **THEN** the test runs in a system Thread whose badge reads Skill test and whose origin link opens that Skill

### Requirement: Smart Views
The inbox SHALL provide the views Unread, Action, Running, All and Archive.
- **Unread:** Threads with unseen results.
- **Action:** Threads with at least one Action item, including Threads whose Run is interrupted.
- **Running:** Threads with a Run in state queued or running.
- **All:** every Thread that is not archived, whether in the Inbox or a folder.
- **Archive:** archived Threads.
Unread and Action show a count badge when the count is greater than zero.

#### Scenario: Thread needs approval
- **WHEN** a Run is interrupted for an Approval
- **THEN** its Thread appears in the Action view and the Action count increases by one

#### Scenario: Running view spans all Threads
- **WHEN** three Threads have active Runs, including Runs started by Triggers while the window was closed
- **THEN** the Running view lists all three Threads after the window opens

### Requirement: Folders Orthogonal To Smart Views
The inbox SHALL let users create, rename and delete folders. Each Thread belongs to at most one folder; a Thread without a folder is in the Inbox. Moving a Thread into a folder MUST NOT remove it from the Unread, Action or Running views. Deleting a folder moves its Threads to the Inbox.

#### Scenario: Filed Thread still demands attention
- **WHEN** a Thread in folder "Project X" gets an Action item
- **THEN** it appears in the Action view as well as in folder "Project X"

### Requirement: Pinned Threads And Date Grouping
The inbox SHALL show pinned Threads first. It then groups the remaining Threads by last-activity period: Today, Yesterday, This week, Last week, This month and Older. Older periods start collapsed.

#### Scenario: Pin a Thread
- **WHEN** the user pins a Thread
- **THEN** it is listed in the Pinned group above all date groups in every view it belongs to

### Requirement: Read State
Opening a Thread SHALL mark it read. A Thread SHALL become unread again when a Run in it completes or fails while it is not the Thread in focus. Users can mark any Thread unread manually.

#### Scenario: Result arrives in background
- **WHEN** a Run completes in a Thread that is not open
- **THEN** the Thread is marked unread and appears in the Unread view

#### Scenario: Mark unread
- **WHEN** the user marks an open Thread unread
- **THEN** it reappears in the Unread view and stays unread after the user moves to another Thread

### Requirement: Archive And Restore
Users SHALL be able to archive a Thread, which removes it from Unread and All, and to restore it from Archive. A new Action item or a new Run in an archived Thread MUST un-archive it automatically, back to its folder or the Inbox.

#### Scenario: Archive finished work
- **WHEN** the user archives a Thread
- **THEN** it disappears from All and Unread and is listed in Archive

#### Scenario: Archived Thread reactivated
- **WHEN** a Trigger appends a new Run to an archived Thread
- **THEN** the Thread leaves the Archive and appears in All, in its folder or the Inbox

### Requirement: Delete With Confirmation
Deleting a Thread SHALL require confirmation and SHALL remove the Thread and its derived data through the forget cascade of client/local-storage.

#### Scenario: Delete Thread
- **WHEN** the user deletes a Thread and confirms
- **THEN** the Thread, its attachments and its derived index entries are removed

### Requirement: Three-Pane Layout
The main window SHALL show the Thread list, the open Thread's feed and a toggleable Activity rail side by side. Below a width of 1180 pixels, the Activity rail SHALL collapse into a drawer.

#### Scenario: Narrow window
- **WHEN** the window is narrower than 1180 pixels
- **THEN** the Activity rail is hidden and opens as a drawer on demand

### Requirement: Streaming Thread Feed
The Thread feed SHALL render model output as it streams. Each tool call appears as a collapsible card showing the tool id, its risk class, its arguments and its result or error. Reasoning content, when provided, appears in a collapsed block.

#### Scenario: Tool call display
- **WHEN** the agent calls files.read during a Run
- **THEN** the feed shows a collapsed card labelled with the tool id, which expands to show arguments and result

### Requirement: Activity Rail
The Activity rail SHALL list the subagent Runs of the open Thread. Each entry shows its Run state as defined by client/agent-runtime (queued, running, interrupted, succeeded, failed or cancelled), its title and its duration. Parallel subagent Runs are grouped, and each transcript is loaded only when the user expands that entry.

#### Scenario: Parallel delegations
- **WHEN** the agent starts three subagents in parallel
- **THEN** the rail shows one group containing three separate entries, each with its own status

### Requirement: Approval Card
For each pending Approval, the feed SHALL show an Approval card. The card offers:
- Approve;
- Edit arguments, then approve;
- Reject;
- Respond instead, where the user's text is used in place of the tool result.

The card also shows the tool, the risk class, the arguments, the agent's stated rationale, and the provenance of any untrusted inputs that led to the call.

#### Scenario: Approval with provenance
- **WHEN** the agent requests mail.send after reading a web page
- **THEN** the Approval card shows the agent's rationale and lists that web page as an untrusted input behind the request

#### Scenario: Edit then approve
- **WHEN** the user edits the arguments of a pending call and approves
- **THEN** the Run resumes with the edited arguments, and the feed shows that the arguments were changed by the user

### Requirement: Question Card
For each pending Question, the feed SHALL show a Question card with the question and its choices. Questions from ask_user also accept a free-text answer. Client-raised Questions offer only their fixed choices. The Run, or the capability that raised the Question, continues after the user answers.

#### Scenario: Agent asks a question
- **WHEN** the agent asks "Which project folder should I use?" with two choices
- **THEN** the Thread shows a Question card with both choices and a text field, and the Thread is listed in the Action view until it is answered

#### Scenario: Client-raised question
- **WHEN** computer use stops because the user moved the mouse
- **THEN** the Thread shows a Question card with exactly the choices Continue and Stop, and no free-text field

### Requirement: Standalone Action Items
A client capability SHALL be able to raise a Question without an active Run, for example a mailbox connection request or a Remote task acceptance. It does this by creating a Thread, or using an existing one, that holds the Question. The Thread appears in the Action view, and the answer is returned to the capability that raised it.

#### Scenario: Mailbox needs connecting
- **WHEN** client/mail-calendar raises "Connect your mailbox" with the choices Connect and Not now
- **THEN** a Thread holding that Question appears in the Action view, and choosing Connect hands control back to client/mail-calendar

### Requirement: Composer
The composer SHALL accept text and attachments through a picker, paste or drag-and-drop. It keeps a separate draft per Thread across restarts, and lists available skills when the user types `/` at the start of a message.

#### Scenario: Draft survives restart
- **WHEN** the user types a draft in a Thread and restarts the client
- **THEN** the draft and its attachments are still in that Thread's composer

#### Scenario: Skill invocation
- **WHEN** the user types `/` in an empty composer
- **THEN** a filterable list of skills available under the user's policy appears, and selecting one inserts its invocation

### Requirement: Queue Or Steer While Running
While a Run is active in the Thread, pressing Enter SHALL queue the message for the next turn and show it in a removable queued list. Pressing Ctrl+Enter SHALL send it as steering input to the active Run. A Stop control SHALL cancel the active Run.

#### Scenario: Queue a follow-up
- **WHEN** the user presses Enter while a Run is active
- **THEN** the message appears in the queued list and is sent when the Run finishes, unless the user removes it first

#### Scenario: Stop
- **WHEN** the user presses Stop
- **THEN** the active Run is cancelled, and the feed marks the turn as stopped by the user

### Requirement: Fork From Message
Users SHALL be able to fork a Thread from any message. Forking creates a new Thread with the fork origin badge, containing the history up to that message and a link back to the source Thread.

#### Scenario: Fork
- **WHEN** the user chooses "Fork from here" on the third message of a Thread
- **THEN** a new Thread opens containing the first three messages and showing a link to the original Thread

### Requirement: Error Banners With Remedy
When a Run fails with an error code defined by client/model-access, the Thread SHALL show an error banner with the remedy for that code:
- AUTH_EXPIRED: reconnect (re-authenticate);
- RATE_LIMIT and MODEL_UNAVAILABLE: retry;
- CONTEXT_LENGTH: fork or compact;
- TIMEOUT: retry or simplify the request;
- QUOTA_EXCEEDED: show when the quota resets.

#### Scenario: Context too long
- **WHEN** a Run fails with CONTEXT_LENGTH
- **THEN** the banner offers "Fork from here" and "Compact Thread", the latter using the manual compaction of client/agent-runtime

### Requirement: Desktop Notifications
The client SHALL show Windows toast notifications when a Run completes, fails or raises an Action item. Notifications are suppressed for the Thread currently in focus and never shown for user cancellation. Each kind can be toggled in settings, and clicking a notification opens its Thread. An organization can fix a toggle with policy.notifications.completed, policy.notifications.failed or policy.notifications.action (default true); a fixed toggle is shown as managed.

#### Scenario: Action notification
- **WHEN** a Run in a background Thread is interrupted for an Approval and Action notifications are enabled
- **THEN** a toast appears, and clicking it opens that Thread at the Approval card

#### Scenario: Cancel is silent
- **WHEN** the user stops a Run
- **THEN** no notification is shown

### Requirement: Full-Text Search
The inbox SHALL provide full-text search across Thread titles and messages, including archived Threads. Results show highlighted snippets, and opening a result scrolls to the matching message and highlights it.

#### Scenario: Jump to message
- **WHEN** the user opens a search result
- **THEN** its Thread opens scrolled to the matching message, and the message is briefly highlighted

### Requirement: Origin Links
Each Thread SHALL link to what created it:
- a Trigger-created Thread links to its Trigger and Occurrence;
- a Remote task Thread links to the Remote task and its issuer;
- a meeting-capture Thread links to its calendar event and recording session;
- a forked Thread links to its source Thread.

#### Scenario: Trigger-created Thread
- **WHEN** the user opens a Thread created by a scheduled Trigger
- **THEN** the header shows the Trigger name and occurrence time as a link to that Trigger's run history

### Requirement: Keyboard Shortcuts
The inbox SHALL be fully operable from the keyboard. It provides at least these shortcuts:
- Ctrl+N: new Thread
- Ctrl+F: search
- Ctrl+B: toggle Activity rail
- Ctrl+,: settings
- E: archive
- U: mark unread
- Up/Down: navigate the list
- Enter: open
- Delete: delete with confirmation
- Esc: back to the list
- Ctrl+/: shortcut help

#### Scenario: Triage by keyboard
- **WHEN** the focus is on a Thread in the list and the user presses E
- **THEN** the Thread is archived and focus moves to the next Thread

### Requirement: Status Bar
The window SHALL show a status bar with:
- the connection state reported by client/control-channel;
- the Policy bundle version in force;
- the health of configured MCP servers;
- a context-usage indicator for the open Thread. It warns at 75% and offers "Compact now". It shows when client/agent-runtime compacts automatically.

#### Scenario: Disconnected
- **WHEN** client/control-channel reports the disconnected state
- **THEN** the status bar shows "disconnected", and the composer shows that new Runs are unavailable

### Requirement: Settings Structure
Settings SHALL be organized into these sections:
- General
- Notifications
- Files (folder grants)
- Connections (MCP servers and the agent browser)
- Mail and calendar
- Skills
- Automations
- Knowledge sources
- Meetings
- Privacy (effective policy summary, telemetry viewer, data deletion)
- About

Settings fixed by policy MUST be shown as managed and read-only. Each capability defines the content of its own section.

#### Scenario: Managed setting
- **WHEN** policy fixes the telemetry level
- **THEN** the setting is shown with a "managed by your organization" label and cannot be changed

### Requirement: Localization
The UI SHALL be available in Czech, German and English. It follows the Windows display language by default, and the user can override the language in settings.

#### Scenario: Czech Windows
- **WHEN** the Windows display language is Czech
- **THEN** all UI text, dates and numbers are shown in Czech format

### Requirement: Accessibility
The UI SHALL meet WCAG 2.1 AA. Every function must be operable by keyboard alone, every control must be exposed to screen readers with a role and name, and Windows high-contrast themes must be respected.

#### Scenario: Screen reader announces Action item
- **WHEN** a new Approval card appears while a screen reader is running
- **THEN** the screen reader announces that an approval is required, along with the tool's name
