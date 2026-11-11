# Spec Delta

## MODIFIED Requirements

### Requirement: Work Details Visibility
By default the Thread feed SHALL show only messages, answers, Action items, errors, the images a Run produced and the sources of Cited Sources In The Feed. Reasoning, tool-call cards and token usage are hidden. While a Run is active, one status line names its current step. A Run that is finished or waiting for the user offers Show details, which reveals its reasoning, tool-call cards and token usage. The General setting Show work details (default off) shows them for every Run.

#### Scenario: Default feed
- **WHEN** a Run reads two files, reasons, and answers, with Show work details off
- **THEN** the feed shows the user's message and the answer, with no reasoning text, no tool cards and no token usage, and while the Run was active it showed a status line such as "Reading files"

#### Scenario: Details for one Run
- **WHEN** the user chooses Show details on a finished Run
- **THEN** that Run's reasoning block, tool-call cards and token usage appear, and other Runs stay compact

#### Scenario: Setting on
- **WHEN** the user turns on Show work details in Settings > General
- **THEN** every Run shows its reasoning blocks, tool-call cards and token usage

### Requirement: Message Text Selection
The text of a message in the Thread feed and in a Chat SHALL be selectable with the pointer, across paragraphs, list items and code blocks of one message. A press on the text gives the message keyboard focus, so Ctrl+C copies the selection as plain text. A click elsewhere clears it, and so does a change to that message's own text; other live updates keep it. Copy as an icon button copies the whole message as written. Links still open when clicked.

#### Scenario: Copy part of a Chat message
- **WHEN** the user drags across two words of a Chat message and presses Ctrl+C
- **THEN** the clipboard holds exactly those two words

#### Scenario: Copy a whole answer
- **WHEN** the user chooses Copy on an answer that contains a list and a code block
- **THEN** the clipboard holds the answer's text as written, with its Markdown

#### Scenario: Link still opens
- **WHEN** the user clicks a link inside a message without dragging
- **THEN** the link's target is shown and opens as before, and no text is selected

#### Scenario: Selection survives a live update
- **WHEN** the user has selected words of an answer and a tool result of the same Run arrives
- **THEN** the selection stays, and Ctrl+C still copies those words

## ADDED Requirements

### Requirement: Cited Sources In The Feed
When an answer cites knowledge passages by their numbers in square brackets, such as [2], the Thread feed SHALL list under that answer only the passages it cites, as a numbered list. Each entry shows its number, title, Source and date, and opens the passage as a citation. A passage the answer does not cite is not listed, and an answer without citations shows no list.

#### Scenario: Only cited passages
- **WHEN** a Run's knowledge.search returns eight passages and the answer cites [2] and [5]
- **THEN** the list under the answer shows exactly passage 2 and passage 5, in number order

#### Scenario: Thread passage cited
- **WHEN** a passage from a finished Thread was returned and the answer cites it as [4]
- **THEN** the list under the answer shows passage 4 with its Thread title, Source and date

#### Scenario: Thread passage not cited
- **WHEN** a passage from a finished Thread was returned but the answer does not cite it
- **THEN** no entry for that passage appears in the feed

#### Scenario: Answer without citations
- **WHEN** the answer contains no citation markers
- **THEN** the feed shows no sources list, and the search result remains in its tool card under Show details

### Requirement: Live Feed Updates
The Thread feed SHALL show each answer once: when the stored answer of a Run arrives, the streamed text of that Run is dropped. While text streams, the feed SHALL redraw at most 30 times per second, and only when it shows the Thread that is streaming. Live updates SHALL keep the reading position and the selection of every message whose text did not change.

#### Scenario: Reasoning before the answer
- **WHEN** a Run streams reasoning and then answers
- **THEN** the Thread feed shows the answer once, with no streamed copy beside the stored one

#### Scenario: Other Thread streams
- **WHEN** a Run in a Thread that is not open streams text
- **THEN** the feed of the open Thread is not redrawn for it
