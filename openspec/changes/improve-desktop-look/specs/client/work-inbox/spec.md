# Spec Delta

## MODIFIED Requirements

### Requirement: Fork From Message
Users SHALL be able to fork a Thread from any message. Each stored message offers Fork from here as an icon button, with the name and tooltip "Fork from here". Forking creates a new Thread with the fork origin badge, containing the history up to that message and a link back to the source Thread.

#### Scenario: Fork
- **WHEN** the user chooses Fork from here on the third message of a Thread
- **THEN** a new Thread opens containing the first three messages and showing a link to the original Thread

#### Scenario: Icon with a name
- **WHEN** a screen reader user moves to the Fork from here control of a message
- **THEN** it is announced as the button "Fork from here", and pointing at it shows the same tooltip

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

## ADDED Requirements

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