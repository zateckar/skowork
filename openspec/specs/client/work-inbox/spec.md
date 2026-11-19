# client/work-inbox Specification

## Purpose
How agent work reaches the user: Threads and their feed, the Inbox views, and how approvals, questions and results are presented.

## Requirements

### Requirement: Images In The Thread Feed
The Thread feed SHALL show image attachments that a tool stored, such as images from image.generate, inline at the point of the tool call, scaled to the feed width. The user can open an image at full size and save it to a location they choose.

#### Scenario: Generated image appears
- **WHEN** image.generate stores an image during a Run
- **THEN** the feed shows the image under that tool call's card without the user expanding the card

#### Scenario: Save an image
- **WHEN** the user chooses Save as on an image in the feed
- **THEN** the client asks for a location and writes the image there

### Requirement: Policy Page
Settings SHALL include a Policy page that shows the summary required by client/policy-enforcement Policy Visibility To User, covering Groups, version and expiry, model roles, tool decisions and modes, filesystem bounds, network rules and enforcement level, and unattended rules. Each setting shows the Group or guardrail it comes from where the bundle says so. The page offers Refresh, which fetches the policy now.

#### Scenario: Computer use off shown
- **WHEN** the user opens the Policy page while policy.tools.computer.mode is off
- **THEN** the page shows computer use as off, with the source of that setting

### Requirement: Files Settings
The Files section of Settings SHALL list the user's folder grants and let the user add a folder through the system folder picker, choose read-only or read-write, and revoke a grant, within the bounds of client/policy-enforcement User Folder Grants. A refused grant shows the explanation that names the policy.

#### Scenario: Grant a folder
- **WHEN** the user adds a folder inside policy.filesystem.grantable as read-only
- **THEN** the grant is listed, and the agent can read files in that folder

### Requirement: Generated Thread Surfaces
The Thread feed SHALL render supported, complete generated surfaces embedded in assistant messages, alongside their text, using native components. Surfaces SHALL support composable sections, cards, read-only text, tables, bar and line charts, and directed relationship diagrams.

#### Scenario: Visual comparison
- **WHEN** an assistant message contains a complete supported surface with a table and a bar chart
- **THEN** the Thread shows both visualizations at that point in the answer with the surrounding text

#### Scenario: Non-assistant content
- **WHEN** a user message, Chat message or tool result contains surface syntax
- **THEN** the feed treats it as content and does not activate a surface

### Requirement: Local Surface Exploration
The Thread feed SHALL let the user sort and filter a generated table and expand or collapse generated sections locally, without sending the interaction to the model.

#### Scenario: Sort without a model request
- **WHEN** the user sorts a generated table by a column
- **THEN** the rows are reordered in the Thread without starting or changing a Run

### Requirement: Generated Feedback Controls
Generated surfaces SHALL offer labeled buttons, single and multiple selections, switches, text fields and sliders, and SHALL submit their user-entered values only on an explicit user action. Submissions SHALL visibly identify the selections being sent.

#### Scenario: Adjust before submitting
- **WHEN** the user changes a switch and edits a text field
- **THEN** the values change locally and no response is sent until the user submits

### Requirement: Generated Surface Fallback
The Thread feed SHALL keep surrounding answer text readable and provide a readable fallback for an incomplete, invalid, oversized or unsupported generated surface. Such a surface MUST NOT expose active generated controls.

#### Scenario: Interrupted stream
- **WHEN** a response stops before a generated surface is complete
- **THEN** its text remains available as fallback and no controls from the incomplete surface can be activated

#### Scenario: Open a Thread after downgrade
- **WHEN** a user opens previously stored Threads with an older client after generated surfaces have been used
- **THEN** the older client can open the existing local data and display the original answer as readable text or code

### Requirement: Generated Surface Setting
The client SHALL provide a disabled-by-default setting for generated surfaces. Changes SHALL apply on the next client restart, and the setting SHALL explain this to the user. When disabled, the client SHALL retain the original readable response and SHALL NOT activate generated controls or request generated surfaces.

#### Scenario: Enable generated surfaces
- **WHEN** the user enables generated surfaces in Settings and restarts the client
- **THEN** eligible responses can display generated surfaces and the runtime can request them

#### Scenario: Disable generated surfaces
- **WHEN** the user disables generated surfaces and restarts the client
- **THEN** existing responses remain readable as text and their generated controls are inactive

### Requirement: Accessible Generated Surfaces
Generated surfaces SHALL support keyboard and assistive technology access, the client's themes and localization, and a text or table alternative for each chart and diagram. Chart alternatives SHALL include values, units and labels supplied by the surface.

#### Scenario: Chart without visual access
- **WHEN** a screen-reader user encounters a generated chart
- **THEN** its accessible alternative exposes its title, values, labels and supplied units

### Requirement: Generated Surface Evidence
Generated surfaces SHALL expose valid Citations associated with displayed values or content through the Thread's existing Citation interaction, and SHALL NOT present an unknown Citation identifier as a verified source.

#### Scenario: Referenced table value
- **WHEN** a generated table cell references an existing Citation
- **THEN** the user can open that Citation from the cell's displayed context

### Requirement: Generated Surface State
The Thread feed SHALL retain unsent input and local exploration state for a generated surface across streaming updates, navigation away and client restart. A submitted, superseded or unavailable surface SHALL show its status and prevent duplicate submission.

#### Scenario: Resume an unfinished response
- **WHEN** the user changes a form value, leaves the Thread and returns after restarting the client
- **THEN** the unfinished value is restored and has not been submitted

#### Scenario: Delete after downgrade
- **WHEN** a Thread containing unsent generated-surface data is deleted by an older client
- **THEN** its generated-surface data is deleted with the Thread and is not exposed as an ordinary attachment

#### Scenario: Duplicate activation
- **WHEN** a user activates the same submit control twice for the same surface revision
- **THEN** at most one submission is accepted

### Requirement: Generated Surface Limits
The client SHALL enforce finite limits on generated surface size, nesting, control count and data points before rendering, and SHALL not execute generated code or fetch network resources described by a surface.

#### Scenario: Oversized surface
- **WHEN** a generated surface exceeds a supported limit
- **THEN** the client shows its fallback without executing it or fetching external content

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
- **Unread:** Threads with unseen results, and Chats with unread messages.
- **Action:** Threads with at least one Action item, including Threads whose Run is interrupted.
- **Running:** Threads with a Run in state queued or running.
- **All:** every Inbox entry not archived, in the Inbox or a folder.
- **Archive:** archived Inbox entries.
Unread and Action show a count badge when the count is greater than zero.

#### Scenario: Thread needs approval
- **WHEN** a Run is interrupted for an Approval
- **THEN** its Thread appears in the Action view and the Action count increases by one

#### Scenario: Running view spans all Threads
- **WHEN** three Threads have active Runs, including Runs started by Triggers while the window was closed
- **THEN** the Running view lists all three Threads after the window opens

#### Scenario: Unread Chat
- **WHEN** a message arrives in a Chat the user does not have open
- **THEN** the Chat appears in the Unread view and the Unread count includes it

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
The Thread feed SHALL render model output as it streams. Each tool call has a collapsible card showing the tool id, its risk class, its arguments and its result or error, and reasoning content, when provided, has a collapsed block. These cards and blocks are shown only as Work Details Visibility allows.

#### Scenario: Tool call display
- **WHEN** the agent calls files.read during a Run and work details are shown for that Run
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
Users SHALL be able to fork a Thread from any message. Each stored message offers Fork from here as an icon button, with the name and tooltip "Fork from here". Forking creates a new Thread with the fork origin badge, containing the history up to that message and a link back to the source Thread.

#### Scenario: Fork
- **WHEN** the user chooses Fork from here on the third message of a Thread
- **THEN** a new Thread opens containing the first three messages and showing a link to the original Thread

#### Scenario: Icon with a name
- **WHEN** a screen reader user moves to the Fork from here control of a message
- **THEN** it is announced as the button "Fork from here", and pointing at it shows the same tooltip

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

### Requirement: Work Details Visibility
By default the Thread feed SHALL show only messages, answers, Action items, errors, and the images and cited sources a Run produced. Reasoning, tool-call cards and the Run's token usage are hidden. While a Run is active, one status line names its current step in plain words. A Run no longer active (finished, or waiting for the user) offers Show details, which reveals its reasoning, tool-call cards and token usage. The General setting Show work details (default off) shows them for every Run.

#### Scenario: Default feed
- **WHEN** a Run reads two files, reasons, and answers, with Show work details off
- **THEN** the feed shows the user's message and the answer, with no reasoning text, no tool cards and no token usage, and while the Run was active it showed a status line such as "Reading files"

#### Scenario: Details for one Run
- **WHEN** the user chooses Show details on a finished Run
- **THEN** that Run's reasoning block, tool-call cards and token usage appear, and other Runs stay compact

#### Scenario: Setting on
- **WHEN** the user turns on Show work details in Settings > General
- **THEN** every Run shows its reasoning blocks, tool-call cards and token usage

### Requirement: Chat Entries
The inbox SHALL list every Chat of client/user-messaging as an Inbox entry next to the Threads, ordered with them by last activity. A Chat entry shows the Chat name, the last message, the relative time, its unread count, the other user's Presence for a direct Chat, and a Chat badge. Opening it SHALL show the conversation with its composer in the reading pane. Chat entries never appear in Action or Running, and are not listed while policy.messaging.mode is off.

#### Scenario: Open a Chat entry
- **WHEN** the user opens the Chat entry of a direct Chat with bob from the Unread view
- **THEN** the reading pane shows the conversation and its composer, and the entry leaves Unread

### Requirement: Chat Entry Triage
The user SHALL be able to pin a Chat entry, move it to a folder or back to the Inbox, and archive and restore it, as for Threads. A new message in an archived Chat un-archives it, back to its folder or the Inbox. Chat entries cannot be deleted; leaving the Chat removes its entry.

#### Scenario: File a Chat in a folder
- **WHEN** the user moves the team Chat "Falcon team" to the folder "Project Falcon"
- **THEN** the folder lists the Chat next to its Threads, and the Chat still appears in Unread while it has unread messages

#### Scenario: Archived Chat comes back
- **WHEN** a colleague writes in a Chat the user archived
- **THEN** the Chat leaves the Archive and appears in All and Unread, in its folder or the Inbox

### Requirement: Message Text Selection
The text of a message in the Thread feed and in a Chat SHALL be selectable with the pointer. A drag selects text, also across the paragraphs, list items and code blocks of one message. Ctrl+C copies the selection as plain text, and a click elsewhere clears it. Each message also offers Copy as an icon button, which copies the whole message as written. Links in the message still open when clicked.

#### Scenario: Copy part of a Chat message
- **WHEN** the user drags across two words of a Chat message and presses Ctrl+C
- **THEN** the clipboard holds exactly those two words

#### Scenario: Copy a whole answer
- **WHEN** the user chooses Copy on an answer that contains a list and a code block
- **THEN** the clipboard holds the answer's text as written, with its Markdown

#### Scenario: Link still opens
- **WHEN** the user clicks a link inside a message without dragging
- **THEN** the link's target is shown and opens as before, and no text is selected

### Requirement: Brand Colours
The desktop app SHALL use the brand colours Emerald (#0E3A2F) and Electric Green (#78FAAE) as its accent instead of the Windows accent colour. It marks the current navigation entry and the selected list row with the accent, shows counts and states as badges, gives actions icons, and shows the sender's initials as an avatar on messages. High-contrast themes use the system colours only.

#### Scenario: Brand accent regardless of the Windows accent
- **WHEN** the Windows accent colour is grey and the app runs in the dark theme
- **THEN** primary buttons, the current navigation entry and count badges use Electric Green

#### Scenario: High contrast
- **WHEN** Windows uses a high-contrast theme
- **THEN** the app draws with the system colours only, without the brand colours and without dimming

#### Scenario: Own messages in a Chat
- **WHEN** the user opens a Chat with messages from them and from a colleague
- **THEN** each message shows the sender's initials, and the user's own messages are set apart from the colleague's

### Requirement: Colour Contrast
The palettes of the light and the dark theme SHALL meet WCAG 2.1 AA: text, including secondary text, link text and text on the accent or on a selection, has at least 4.5:1 against the colours it is drawn on, and the boundary of a control (buttons, fields, checkboxes) and the focus indicator have at least 3:1 against the surface they sit on.

#### Scenario: Contrast
- **WHEN** the palette of the light or the dark theme is checked
- **THEN** every text colour reaches 4.5:1 and every control boundary 3:1 against the colours it is drawn on

### Requirement: Dialogs And Action Rows
A dialog SHALL dim the window behind it by darkening it, and no dialog or row of actions cuts off a control: buttons that do not fit on one line continue on the next.

#### Scenario: Narrow dialog
- **WHEN** a dialog has more buttons than fit in its width
- **THEN** the buttons continue on a second line inside the dialog, and all of them can be seen and clicked

#### Scenario: Dimming in the dark theme
- **WHEN** a dialog opens in the dark theme
- **THEN** the window behind it is darker than before, not lighter

### Requirement: Markdown Rendering
The Thread feed SHALL render model output as Markdown (CommonMark with tables, strikethrough and task lists), also while it streams. Code blocks use a monospace font and offer Copy. Model output is untrusted: raw HTML in it is shown as text, images in it are never fetched and are shown as their alternative text with the link, and a link opens in the default browser only when the user clicks it, after the target URL is shown.

#### Scenario: Formatted answer
- **WHEN** the model answers with a heading, a bulleted list, a table and a code block
- **THEN** the feed shows them formatted, and the Markdown markup itself is not visible

#### Scenario: Remote image in output
- **WHEN** model output contains a Markdown image with a remote URL
- **THEN** no request is made to that URL, and the feed shows the alternative text and the link

#### Scenario: Raw HTML in output
- **WHEN** model output contains an HTML tag such as a script element
- **THEN** the feed shows it as literal text

### Requirement: Agenda View
When the Agenda is enabled (client/agenda), the Inbox views SHALL include an Agenda view listing open Agenda items grouped as Overdue, Today, Tomorrow, This week and Later, sorted by due time. Each row SHALL show the title, the due date with an inferred marking where applicable, the kinds of its sources and whether a preparation exists, and SHALL offer Prepare or Open preparation, Open source, Mark done, Dismiss and Edit date. Every row and action SHALL have a UI Automation name.

#### Scenario: Open the Agenda
- **WHEN** the user has two open items due today and one due next week
- **THEN** the Agenda view shows them under Today and Later, and each row offers Prepare

#### Scenario: Agenda disabled
- **WHEN** the Agenda is disabled
- **THEN** the Agenda view is not listed among the Inbox views
