# Spec Delta

## MODIFIED Requirements

### Requirement: Markdown Rendering
The Thread feed SHALL render model output as Markdown (CommonMark with tables, strikethrough and task lists), also while it streams. While text streams, an unfinished emphasis, strikethrough, code span or link at the end of the text is shown as if it were closed, so its markup never appears and then disappears; a link that is not yet complete shows only its text. Code blocks use a monospace font and offer Copy. Model output is untrusted: raw HTML in it is shown as text, images in it are never fetched and are shown as their alternative text with the link, and a link opens in the default browser only when the user clicks it, after the target URL is shown.

#### Scenario: Formatted answer
- **WHEN** the model answers with a heading, a bulleted list, a table and a code block
- **THEN** the feed shows them formatted, and the Markdown markup itself is not visible

#### Scenario: Unfinished emphasis while streaming
- **WHEN** the streamed text so far ends with "the **important" and the rest has not arrived
- **THEN** the feed shows "the important" with "important" in bold, and no asterisks

#### Scenario: Unfinished link while streaming
- **WHEN** the streamed text so far ends with "see [the report](https://exa"
- **THEN** the feed shows "see the report" without the brackets or the partial address, and nothing can be opened yet

#### Scenario: Remote image in output
- **WHEN** model output contains a Markdown image with a remote URL
- **THEN** no request is made to that URL, and the feed shows the alternative text and the link

#### Scenario: Raw HTML in output
- **WHEN** model output contains an HTML tag such as a script element
- **THEN** the feed shows it as literal text

### Requirement: Streaming Thread Feed
The Thread feed SHALL render model output as it streams. Each tool call has a collapsible card showing the tool id, its state, its risk class, its arguments and its result or error. The card's header shows how long the call took once it has a result, and how long it has been running before that. A result that is a JSON object or array is shown indented. Reasoning content, when provided, has a collapsed block whose header says "Thinking" with the time so far while it streams, and "Thought for" its stored duration afterwards; it says "Thinking" or "Reasoning" without a time while that time is unknown or under one second. These cards and blocks are shown only as Work Details Visibility allows.

#### Scenario: Tool call display
- **WHEN** the agent calls files.read during a Run and work details are shown for that Run
- **THEN** the feed shows a collapsed card labelled with the tool id, which expands to show arguments and result

#### Scenario: Tool call duration
- **WHEN** a web.fetch call returns its result 1.4 seconds after it was made and work details are shown
- **THEN** the card's header shows its result state and "1.4 s", and a screen reader reads both with the tool id

#### Scenario: JSON result
- **WHEN** a tool returns the result {"count":2,"items":["a","b"]}
- **THEN** the expanded card shows it on several indented lines

#### Scenario: Reasoning duration
- **WHEN** a model call streamed reasoning for 8 seconds and work details are shown
- **THEN** the reasoning block's header says "Thinking" with the seconds counting up while it streams, and "Thought for 8 s" after it is stored

### Requirement: Composer
The composer SHALL accept text and attachments through a picker, paste or drag-and-drop. It keeps a separate draft per Thread across restarts, and lists available skills when the user types `/` at the start of a message. Send, Queue and Steer are disabled while the draft is blank and has no attachments; while a Run is active and they are disabled, Stop is the primary button.

#### Scenario: Draft survives restart
- **WHEN** the user types a draft in a Thread and restarts the client
- **THEN** the draft and its attachments are still in that Thread's composer

#### Scenario: Skill invocation
- **WHEN** the user types `/` in an empty composer
- **THEN** a filterable list of skills available under the user's policy appears, and selecting one inserts its invocation

#### Scenario: Empty draft
- **WHEN** the composer of an idle Thread holds only spaces and no attachment
- **THEN** Send is disabled and pressing Enter sends nothing

#### Scenario: Empty draft while running
- **WHEN** a Run is active and the composer is empty
- **THEN** Queue and Steer are disabled and Stop is shown as the primary button

### Requirement: Keyboard Shortcuts
The inbox SHALL be fully operable from the keyboard. It provides at least these shortcuts:
- Ctrl+N: new Thread
- Ctrl+F: search
- Ctrl+K: command palette
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

#### Scenario: Palette from a text field
- **WHEN** the focus is in the composer and the user presses Ctrl+K
- **THEN** the command palette opens and the composer keeps its draft

### Requirement: Status Bar
The window SHALL show a status bar with:
- the connection state reported by client/control-channel;
- the Policy bundle version in force;
- the health of configured MCP servers;
- a context-usage indicator for the open Thread. It warns at 75 % and offers "Compact now", and shows the error colour from 90 %. Its description lists what fills the context: instructions, tool definitions, conversation, and what is still free. It shows when client/agent-runtime compacts automatically.

#### Scenario: Disconnected
- **WHEN** client/control-channel reports the disconnected state
- **THEN** the status bar shows "disconnected", and the composer shows that new Runs are unavailable

#### Scenario: Context breakdown
- **WHEN** the user hovers over the context indicator, or a screen reader reads it, for a Thread at 62 %
- **THEN** it shows the shares of instructions, tool definitions and conversation and the free share, which add up to 100 %

#### Scenario: Nearly full
- **WHEN** the open Thread's context reaches 92 %
- **THEN** the indicator uses the error colour and still offers "Compact now"

## ADDED Requirements

### Requirement: Step Elapsed Time
While a Run is active, the status line of Work Details Visibility SHALL show how long its current step has been running once that is at least 2 seconds, in whole seconds, counting up once a second while the open Thread is shown. A step starts with a tool call or with streamed reasoning. Waiting for the model is shown by Generation Speed Under Answers and not by the status line.

#### Scenario: Long tool call
- **WHEN** a Run has been reading files for 12 seconds with work details hidden
- **THEN** the status line says "Reading files · 12 s"

#### Scenario: Short step
- **WHEN** a tool call finishes within one second
- **THEN** the status line never shows a time for it

### Requirement: Command Palette
Ctrl+K SHALL open a command palette over the window: a search field and a list of commands and Inbox entries. The commands are the navigation targets and the actions of Keyboard Shortcuts that apply where the user is, each with its shortcut when it has one; the entries are the Inbox entries of the Inbox views, by title. Typing filters the list: an item matches when the typed characters appear in its name in order, ignoring case and accents; items whose name starts with the text rank first, then items with a word starting with it, then the others, commands before entries at equal rank; at most 50 items are shown. Up and Down move the selection, Enter runs the selected item, and Escape or a click outside closes the palette and returns the focus. A screen reader reads the field, the number of results and the selected item.

#### Scenario: Open a Thread by title
- **WHEN** the user presses Ctrl+K, types "budg" and presses Enter while "Budget review Q3" is the first result
- **THEN** that Thread opens and the palette closes

#### Scenario: Run a command
- **WHEN** the user types "sett" in the palette and chooses Settings
- **THEN** the Settings page opens

#### Scenario: Subsequence match
- **WHEN** the user types "nwthr"
- **THEN** "New Thread" is listed

#### Scenario: No match
- **WHEN** nothing matches the typed text
- **THEN** the palette says that nothing was found, and Enter does nothing

### Requirement: Run Plan In The Feed
The Thread feed SHALL show the current Run plan of a top-level Run (client/agent-runtime Run Plan Tool) as one plan card where its first Run plan was made, also when work details are hidden. The card's header says how many steps are done of how many. Each step shows its title and its status in words and with a mark, never by colour alone; the running step is highlighted. When the Run is no longer active, a step still pending or running is shown as not done. The `update_plan` tool calls themselves are tool cards like any other, shown only as Work Details Visibility allows.

#### Scenario: Plan while running
- **WHEN** a Run's latest Run plan has five steps, two done and the third running, with work details hidden
- **THEN** the feed shows one plan card with "2 of 5 done", the third step marked as running, and no `update_plan` tool cards

#### Scenario: Plan changes
- **WHEN** the agent marks the third step done and the fourth running
- **THEN** the same plan card updates in place to "3 of 5 done" without moving

#### Scenario: Run stopped
- **WHEN** the user stops the Run while the fourth step is running
- **THEN** the plan card shows the fourth and fifth steps as not done

#### Scenario: Screen reader
- **WHEN** a screen reader reads the plan card
- **THEN** it reads a list whose items are named like "Step 3 of 5, Draft the reply, running"

### Requirement: Button Feedback
A control whose action gives no other visible result SHALL show that result on itself: Copy controls (a message's Copy, a code block's Copy) show a check mark and "Copied" for 1.5 seconds, or "Copy failed", and the result is also announced to screen readers. Windows toast notifications are not used for the results of the user's own clicks.

#### Scenario: Copy an answer
- **WHEN** the user clicks Copy on an answer
- **THEN** the button shows a check mark and "Copied" for 1.5 seconds and then returns to Copy, and a screen reader announces "Copied"

### Requirement: Stable Avatar Colours
An avatar of a person (the user's own and other users' in Chats and Chat entries) SHALL have a fill colour chosen from a fixed palette of eight by a stable hash of that person's user name, so the same person has the same colour everywhere and across restarts. Every palette colour keeps the initials at a contrast of at least 4.5:1 in the light and the dark theme. The Skowork avatar keeps the brand accent, and a high-contrast theme draws all avatars with the system colours.

#### Scenario: Same colleague, same colour
- **WHEN** a colleague appears in the Chat list and in a team Chat
- **THEN** both avatars have the same fill colour, also after a restart

#### Scenario: High contrast
- **WHEN** Windows uses a high-contrast theme
- **THEN** avatars are drawn with the system colours only

### Requirement: Balanced Line Wrapping
Wrapped interface text (labels, captions, headings, card and dialog text; not message text) SHALL NOT end with a single word alone on its last line when the text wraps onto two or more lines, its last line holds one word, and the word can move down a line without adding a line.

#### Scenario: Orphan word
- **WHEN** a wrapped caption would end with "the" alone on its third line
- **THEN** the caption wraps a word earlier, so its last line has two words and it still has three lines
