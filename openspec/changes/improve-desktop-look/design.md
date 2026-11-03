# Design

## Context

The native window (add-native-ui) draws everything itself with Direct2D, so colours, selection and layout behaviour live in client/src/ui. add-native-ui design D7 made the window follow Windows: light or dark, high contrast, and the Windows accent colour. The product owner's review on 2026-11-02 (proposal) found the result bland. It also found real defects: a dialog cut off its buttons, the dialog dimming greyed the window in dark mode, and control borders had too little contrast.

## Goals / Non-Goals

- **Goals:**
  - a recognisable look with two brand colours;
  - WCAG 2.1 AA contrast, checked by a test;
  - text that can be selected and copied;
  - quieter message actions;
  - a people picker;
  - no clipped controls.
- **Non-Goals:**
  - a theme setting;
  - new layouts for the views;
  - selection across messages.

## Decisions

### D1. Palette
- **Choice:** the brand accent replaces the Windows accent. current_theme() no longer reads the colourisation colour; make_theme(dark, high_contrast, system colours) builds the palette. The neutrals carry a slight green tint so they sit with the accent.
  - Dark:
    - background #131A17, surface #1B2420, alternate surface #17201C;
    - dividers #2C3833, control borders #6E7D77;
    - text #F2F5F4, secondary text #B3C0BB;
    - accent #78FAAE with text #0E3A2F; selection #1F4536 with white text;
    - links #78FAAE, focus ring #78FAAE.
  - Light:
    - background #F1F5F3, surface #FFFFFF, alternate surface #E6EEEA;
    - dividers #D5DFDA, control borders #7A8A84;
    - text #0E1F19, secondary text #46564F;
    - accent #0E3A2F with white text; selection #D3F9E4 (an Electric Green tint) with the text colour;
    - links #0A6B47, focus ring #0E3A2F.
  - Status colours stay distinct from the accent:
    - error, warning and info have their own colours;
    - success uses the accent family.
  - A new paint, control_border, draws the boundary of buttons, fields and checkboxes. The existing border paint stays for dividers and card edges, which are not controls.
  - A new paint, accent_surface, is a surface tinted with the accent: #1E2E27 (dark) and #E6F7EE (light). It carries the user's own messages, the picked-people chips and the origin badges of the Thread list. The contrast test checks every text colour on it.
- **Why:** two colours, as asked; the accent no longer depends on a machine setting; the contrast levels are met by construction and tested.
- **Alternatives considered:**
  - keeping the Windows accent with a fallback, which leaves the look to each machine;
  - a theme setting, which is more to build than was asked.

### D2. Contrast test
- **Choice:** a portable unit test computes the WCAG relative luminance and checks both palettes:
  - text, secondary text, link and selection text against background, surface, alternate surface and selection, where they are drawn: 4.5:1;
  - text on the accent: 4.5:1;
  - control_border and focus against surface and background: 3:1.
- **Why:** contrast is a requirement, so it is checked on every build rather than by eye.

### D3. Wrapping rows
- **Choice:** a Node gains `wraps`. In a wrapping row, children with fit or fixed widths flow onto further lines when the next one does not fit. Fill children (spacers) take what is left of their line, and lines are separated by the row's gap. The dialog footer and the action rows of the views wrap.
- **Why:** this fixes the clipped Save as note dialog and every future row with many buttons, in one place.

### D4. Save as note choices
- **Choice:** the dialog body lists the three periods as full-width choice buttons, each with an icon and a short line. The footer has Cancel only. A choice starts the draft at once, as before.
- **Why:** three equal choices read better as a list than as footer buttons, and the dialog keeps its width.

### D5. Text selection
- **Choice:** selection lives in MarkdownView, the element that renders a message.
  - A press on its text that is not on a link or a button starts a selection. The view takes the pointer until release, and moving extends the selection over its rich-text blocks in document order.
  - Each RichText draws its part of the selection with the selection colour, using DirectWrite hit testing. It shows the text cursor.
  - The selected text joins the blocks with line breaks. The window's Ctrl+C copies the current selection when no text field has focus.
  - Ui keeps the one active selection, so starting another clears it. A view that is destroyed clears its own.
  - The Copy icon on each message copies the message as written (its Markdown).
  - The user's own messages in the Thread feed are shown as written, without Markdown, in a MarkdownView (set_plain), so they are selectable too.
- **Why:**
  - a message is the natural unit;
  - the hit testing comes with the existing text layouts;
  - the keyboard and screen readers keep the Copy button and the value of the message (UI Automation Value), which already holds the plain text.
- **Alternatives considered:** a read-only rich-edit control per message, which costs a child window per message and loses the Markdown rendering.

### D6. Message header and actions
- **Choice:**
  - A message header shows an avatar with the sender's initials (in the Thread feed, the user's are taken from the messaging name, else the sign-in user name), the sender, the time, and at the end the icon buttons Copy and Fork from here (Fork only on stored messages). The agent's avatar is the Workbench mark (an accent circle with "S").
  - In Chats, the user's own messages sit on an accent-tinted surface and the others on the alternate surface. Each has an avatar, the sender and the time.
  - The Chat header actions are icon-and-text buttons in a wrapping row: Rename, Add members, Save as note, Leave.
  - Glyphs (Segoe Fluent Icons): Copy E8C8, Fork EA3C, Rename E8AC, Add members E8FA, Save as note E70B, Leave F3B1, Chat E8BD; the Save as note periods Last 24 hours E8BF, Last 7 days E8C0, Everything E81C.
- **Why:** quieter than text buttons on every message, and still named for screen readers.

### D7. People picker
- **Choice:** a PeoplePicker element in the Chats code. It has:
  - a search field that queries messaging.directory as the user types (two characters or more, at most one request in flight, the latest query wins);
  - a list of matches with the avatar, name, user name and Presence, and a checkbox each (display names can repeat, so the user name tells them apart);
  - chips for the picked users, each with a remove button.

  Add members shows it in a dialog with Add (enabled once someone is picked). New team Chat shows the name field and the picker. Members of the Chat are listed as "member" and are disabled. The directory and Message buttons of the Chats pane stay for direct Chats.
- **Why:** the directory search exists already (control-plane/user-messaging), so no server change is needed.

### D8. Usage line
- **Choice:** the feed treats the usage item as work detail. It is hidden unless Show details is on for that Run, or Show work details is on. A Run whose only hidden item is its usage still offers Show details.
- **Why:** the requirement now names usage as work detail.

### D9. Dimming
- **Choice:** the modal layer fills with black at 40 % (dark) or 30 % (light). High contrast draws no dimming, as before.

### D10. Visual pass
- **Choice:**
  - Navigation: the current entry is drawn with the selection colour, the accent bar and an accent icon; counts are accent badges in the wide and the compact navigation.
  - Thread list: the selected row gets the selection colour and the accent bar; rows are separated by a divider; the Chat and origin badges sit on accent_surface in the text colour.
  - Status bar: a dot before the connection state (success when connected, warning otherwise).
  - A disabled checked checkbox (a member in the picker) is drawn muted, not in the accent.

## Risks / Trade-offs

- [Golden images change everywhere] → They are regenerated once with WORKBENCH_UPDATE_GOLDEN=1, and the new images are reviewed by eye.
- [Users who liked the Windows accent] → Not asked for. A theme setting can follow if requested.
- [Selection conflicts with clicks on links and buttons] → A press on an interactive child is left to that child; selection starts only on text.

## Acceptance (2026-11-02)

On the demo client (debug build, local control plane, signed in through the organization Keycloak), dark theme:
- The Inbox, a Thread, a team Chat, the Save as note dialog and the people picker were captured. They show the brand accent on the current navigation entry, the count badges, the selected list row and the primary buttons. Control boundaries are visible, the dialog dimming darkens the window, and the Save as note choices sit inside the dialog with Cancel alone in the footer.
- Add members by searching: in a team Chat whose only member was the user, typing "pet" listed two colleagues with the same display name, told apart by user name. Picking one made a chip and enabled Add, and Add made Petra Peer a member on the control plane.
- Copy on a Chat message put its Markdown on the clipboard (`Falcon review moved to **Thursday**`).
- Not checked live:
  - Dragging to select text. Desktop input needs the user's consent; the drag, Ctrl+C and a click that clears are covered by UiMarkdown.DragSelectsTextAcrossBlocksAndAClickClears.
  - The light theme. Switching the Windows app mode is a system setting. It is covered by the light golden images (the element gallery and the six shell views, reviewed by eye), the hidden-window shell tests, which run light, and the contrast test of both palettes.
- Test suites: ui_tests 57 passed. win_tests: all passed after the golden images were regenerated. ShellAutomations.EditorRunNowAndOpenResultFromHistory failed once on timing and passed when run alone.

## Open Questions

- None.
