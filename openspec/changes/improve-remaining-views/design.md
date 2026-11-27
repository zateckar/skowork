# Design

## Context

Every screen was rendered offscreen with mocked host data (a temporary preview test in `client/tests/shell/shell_test.cpp`, not committed) and reviewed. Relevant code: `client/src/shell/shell.cpp` (`moment`, `clock`), `client/src/ui/textfield.cpp`, `client/src/ui/elements.cpp` (`Button`, `Checkbox`), `client/src/shell/views_*.cpp`. Note for later reviews: an offscreen capture does not show the text of a field that holds text (its rich-edit child window is not captured), so a filled field looks empty in such images.

## Decisions

### D1. One date and time format, converted per moment
`Shell::moment`, `Shell::clock`, `Shell::day` and `Shell::date_of` convert with `SystemTimeToTzSpecificLocalTime`, which applies the daylight-saving rule of the moment's date; `FileTimeToLocalFileTime` applied today's offset to every date. The user asked for one European format in every language: `HH:mm` and `d.M.yyyy` ("27.11.2026"), built in the shell instead of from the locale. The Calendar, the Agenda due chips and the meeting facts line use the same day.month form without spaces. `Shell::number` groups digits with the language's separators (`GetNumberFormatEx`).
Alternatives: the locale's short formats (rejected by the user: English showed "11/27/2026 2:40 PM").

### D2. Placeholder from the label
`TextField` uses its accessible name as placeholder when no hint is given (not for masked fields). Forms reviewed as a whole put a label above each field; those fields clear the placeholder unless an example helps (cron, time zone, file pattern, Skill frontmatter).
Alternatives: a label above every field everywhere (rejected: single-field rows next to their button read better with a placeholder).

### D3. Measured as drawn
`Button` measures its label with `text_style_`, so the bold chosen tab gets its width. `Checkbox` wraps its label when the available width is smaller than the label, with the box beside the first line.

### D4. Settings layout without new elements
The text of a removable item fills its row (wrapping label), so the action sits at the end. `PagePane::rebuild` gives every sub-heading (a heading label that is not a section title and not first in its container) 8 DIP above it on Status, Settings and Telemetry. Working days and capture sources use the existing wrapping row (`Node::wraps`).

### D5. Words for Policy and Skills
`folder_words` resolves environment variables (`ExpandEnvironmentStringsW`) and reads `\**` as "and its subfolders"; the resolved path goes to the tooltip. A Policy source `group:<name>` reads "Group <name>"; a default source is not shown. Skills list Tools through `Shell::tool_words`, as the Approval card does.

### D6. Previews through the Markdown parser
The list row runs the stored preview through `ui::parse_markdown` and `ui::plain_text` before collapsing whitespace. The store keeps the raw preview; search is unchanged.

### D7. Apps as cards
Apps follow the Agenda card pattern: a `ui::Box`, a badge for Running, Open as the one visible action and a More menu (`app-more-<id>`) whose items keep the former automation ids.

## Risks / Trade-offs

- [A fixed format ignores a user's Windows locale] -> Requested by the user; it is one format across the window and the three languages.
- [A preview cut mid-Markdown keeps stray markers] -> md4c leaves unmatched markers as text; rare and harmless.
- [Moved App actions are found less easily] -> The menu is named "More actions for <App>", as elsewhere.
