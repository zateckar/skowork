# Proposal

## Why

On 2026-11-02 the product owner reviewed the desktop app and asked for these changes:

- Text in Chats cannot be selected or copied.
- "Fork from here" is a text button on every message, which is noisy. It should be an icon.
- The token usage under each answer is detail most users do not need.
- Save as note on a Chat is broken: the dialog's four buttons run past its edge.
- Adding members to a team Chat needs their exact user names. The user should search and pick.
- The app looks bland and is hard to look at. It should use one or two colours, icons and badges, with enough contrast and clear separation between elements.

Several causes come from the toolkit, not from the views:
- The accent colour follows the Windows accent. On the product owner's machine that is grey, so primary buttons, selection and counts barely stand out.
- The dialog dimming is drawn in the text colour. In dark mode that is white, so an open dialog washes the window grey.
- Control borders reach about 1.3:1 against their surface. WCAG 2.1 AA asks 3:1 for the boundary of a control.
- A row of buttons never wraps, so a dialog or an action row with many buttons is cut off.

## What Changes

- **Text selection.** Message text in the Thread feed and in Chats can be selected with the pointer, across the paragraphs of one message, and copied with Ctrl+C. Each message also has a Copy icon that copies its whole text.
- **Message actions as icons.** Fork from here becomes an icon button, and Copy sits next to it. The Chat header actions get icons too (Rename, Add members, Save as note, Leave). Each icon has a tooltip and an accessible name.
- **Usage under Show details.** The token usage line of a Run is work detail. It appears with Show details or the Show work details setting, and no longer by default.
- **Save as note dialog.** The period is a list of three choices inside the dialog, and the footer keeps only Cancel. Rows of buttons now wrap in every dialog and action row, so no control is cut off.
- **People picker.** Add members and New team Chat use a picker: the user types part of a name, picks colleagues from the matches (with their Presence), and sees the picked ones as removable chips. Nobody has to type an exact user name.
- **Visual design.**
  - Two brand colours, Emerald and Electric Green, replace the Windows accent, in light and dark.
  - Neutral colours are tuned so text meets 4.5:1 and control boundaries meet 3:1.
  - Selection and the current navigation entry use the accent.
  - Counts and states are badges.
  - Messages show the sender's initials as an avatar, and in Chats one's own messages are set apart.
  - The dialog dimming darkens instead of greying.
  - High-contrast themes keep the system colours, as before.

Out of scope:
- Selecting text across several messages at once, and selection in other views (lists, settings).
- A theme setting (brand colours or the Windows accent). The app follows Windows light or dark, and the high-contrast themes, as before.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/work-inbox`:
  - Fork From Message (modified);
  - Work Details Visibility (modified);
  - Message Text Selection (added);
  - Brand Colours (added);
  - Colour Contrast (added);
  - Dialogs And Action Rows (added).
- `client/user-messaging`: Chats View (modified).

## Impact

- **Roadmap:** 24, after add-chats-in-inbox. Depends on add-work-inbox, add-native-ui, hide-work-details, add-user-messaging and add-save-as-note.
- **Code:**
  - client/src/ui: the theme palette and a control-border colour; wrapping rows; text selection in rich text; avatars; the dialog dimming;
  - client/src/shell: message headers (Copy, Fork icons, avatars), the usage line as work detail, the Chat header icons, the message look in Chats, the Save as note choices, and the people picker;
  - strings in en, de, cs.
- **Tests:** palette contrast checks, wrapping layout, text selection and copy, the picker, and updated golden images.
- **Data:** none.
