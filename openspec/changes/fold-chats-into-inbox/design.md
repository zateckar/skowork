# Design

## Context

add-chats-in-inbox made every Chat an Inbox entry and kept the Chats navigation entry (its D4) as a Chats-only view of the list, with the busy switch, the user directory and New team Chat in the reading pane. improve-desktop-look added the people picker for team Chats and Add members.

## Decisions

### D1. No Chats entry
- **Choice:** the navigation loses the Chats entry. The view `chats` and the route `#/chats` (without a Chat id) are removed; an old `#/chats` link opens the Inbox. `#/chats/<id>` still opens a Chat in the reading pane.
- **Why:** Chats already sit in every view where the user looks for new work, so the entry only duplicated the list.

### D2. New Chat
- **Choice:**
  - A New Chat icon button (the chat glyph) sits right of New thread in the navigation's top row; on a compact navigation it sits under the New thread button.
  - It opens the dialog "New Chat": the people picker, then a Name field. Start is enabled once someone is picked. With one person picked and no name, it starts a direct Chat (messaging.create kind direct returns the existing one); with two or more it needs a name and starts a team Chat. The Chat opens in the reading pane.
  - The directory search with Message and Select buttons goes away: the picker covers it.
- **Alternatives considered:** a split New button with a menu, which hides Chats behind a second click.

### D3. Busy in the status bar
- **Choice:** app.status.messaging gains `busy`. While messaging is enabled, the status bar shows an inline button with a Presence dot: "Available" (green) or "Busy" (red). Clicking it calls messaging.busy with the other value. Its accessible name says what clicking does ("Available. Set busy").
- **Why:** the status bar is always visible and already holds the user's state (connection, policy).

## Risks / Trade-offs

- [Users who learned the Chats entry] → Chats are in Unread and All with a Chat badge, and New Chat is next to New thread.

## Acceptance (2026-11-02, demo client on the local stack)

- The navigation pane has no Chats entry. New Chat sits next to New thread, and Chat entries (for example Petra Peer, with the Chat badge) are in the Inbox with the Threads.
- The status bar shows "Available" with a green dot. Invoking it through UI Automation set Busy ("Busy. Set available", red dot), and invoking it again set Available.
- New Chat opened the dialog with the people picker and the name field. Start Chat stays disabled until someone is picked. Cancel closed it.
- Tests: ShellChats.EntriesConversationNewChatLeaveAndOff (no Chats entry, `#/chats` opens the Inbox, busy, New Chat direct and team, leave, policy off) and the shell_core route and view tests. Golden images regenerated and reviewed.
- Not checked: that the peer sees the busy change, and creating real Chats on the stack. The demo session has only one signed-in user.