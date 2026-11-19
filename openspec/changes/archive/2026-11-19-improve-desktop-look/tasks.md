## 1. Toolkit

- [x] 1.1 Brand palette and control_border (D1) with the contrast test (D2); the dialog dimming (D9). Verify with ui_tests: both palettes pass the contrast checks, high contrast keeps system colours
- [x] 1.2 Wrapping rows (D3) in layout, used by the dialog footer. Verify with ui_tests: a row of buttons wider than its parent continues on a second line and measures its height
- [x] 1.3 Text selection in MarkdownView and RichText with Ctrl+C (D5). Verify with UI tests: a drag selects across two blocks and copies the plain text; a click on a link opens it without selecting

## 2. Desktop app

- [x] 2.1 Message headers with avatar, Copy and the Fork icon; the usage line as work detail (D6, D8). Verify with hidden-window UI tests: the Fork icon's name, Copy copies the message, usage hidden by default and shown with Show details
- [x] 2.2 Chats: message look, header action icons in a wrapping row, the Save as note choices (D4, D6). Verify with hidden-window UI tests: the save dialog's choices are inside the dialog and start the draft
- [x] 2.3 People picker for Add members and New team Chat (D7). Verify with hidden-window UI tests: search, pick, chips, existing members disabled, Add sends the picked users
- [x] 2.4 The visual pass over navigation, list rows, badges, buttons, status bar and cards with the new paints; strings in en, de, cs; golden images regenerated. Verify with the UI test suites and an a11y check

## 3. Acceptance

- [x] 3.1 On the demo client: screenshots of the Inbox, a Thread, a Chat, the Save as note dialog and the picker in dark and light; select and copy text from a Chat message; add a member by searching. Record the result in design.md
- [x] 3.2 Run `openspec validate --all --strict`
