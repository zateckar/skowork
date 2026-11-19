# Design

## Context

- The Inbox list comes from InboxStore.list(view, cursor). It queries Threads with their inbox state (unread, archived, pinned, folder) and keyset paging on (pinned, last activity, id).
- Chats are cached by the messaging service in app.db, with their last activity and unread count from control-plane/user-messaging.
- The Chats view (#/chats) has its own list and conversation pane.

## Decisions

### D1. Inbox state of Chats in its own table
- **Choice:**
  - Migration 1410 adds `chat_inbox(chat_id PRIMARY KEY, folder_id, pinned, archived, last_activity, title, preview, unread)`.
  - The messaging service rewrites title, preview, last activity, unread, kind and Presence from its cache after every change it reports (and once at start, so Chats cached before this version are listed while offline). A new message from a colleague clears archived; the user's own message does not.
  - Folder deletion moves Chats to the Inbox like Threads.
  - Leaving a Chat or losing it removes its row.
  - With policy.messaging.mode off, no Chat rows are listed.
- **Alternatives considered:**
  - Making each Chat a Thread, which mixes two concepts the glossary keeps apart and gives Chats Runs they cannot have.
  - Joining the messaging cache directly in the list query, which ties the inbox schema to the cache's layout.

### D2. One list, two kinds
- **Choice:**
  - The list query is a UNION of Threads and chat_inbox rows with the same ordering key (pinned desc, last_activity desc, id). Entries carry `kind` (thread or chat).
  - Chat entries take part in Unread (unread > 0), All, Archive, Inbox and folder views, never in Action or Running.
  - Counts.unread counts Threads plus Chats with unread messages.
  - The triage RPCs (threads.pin, threads.archive, threads.move) accept a Chat entry id, prefixed `chat:`, and update chat_inbox. threads.mark_unread, threads.rename, threads.open and threads.delete refuse a Chat entry with invalid_params: a Chat is read when opened, renamed in its conversation, and left rather than deleted.
  - The host emits inbox.changed with the Chat's entry (or `deleted` when it is gone) after each messaging change, so the list updates like it does for Threads.
- **Alternatives considered:** a second list widget for Chats inside the Inbox, which loses one ordering by activity.

### D3. Rows and the reading pane
- **Choice:**
  - A Chat row shows a Chat badge, the name (with a Presence dot for direct Chats), the last message as preview, the relative time and an unread pill.
  - The context menu offers Pin, Move to folder, Archive and Leave Chat. The Delete key on a Chat entry asks to leave it; Mark as unread does nothing for a Chat.
  - Opening it routes to `#/chats/<id>`. That renders the conversation (messages, composer, header actions) in the reading pane, with the list of the current view still on the left. On a narrow window the conversation replaces the list and has a Back button.
  - A note's origin link (add-save-as-note D4) opens the same route.
  - Delete is not offered for Chats; Leave Chat is.
- **Alternatives considered:** navigating away to the Chats page, which loses the Inbox context.

### D4. The Chats entry
- **Choice:**
  - The Chats navigation entry (`#/chats`) opens the Inbox list with the view `chats` (every Chat, not archived).
  - While no Chat is open, the reading pane holds the busy switch, the directory and New team Chat. A Chat started there opens at once.
  - Its count stays the number of unread messages.
  - The directory sits in the reading pane rather than above the list, because the list column (240-420 DIPs) is too narrow for the search field, its button and New team Chat.
- **Alternatives considered:** removing the entry, which leaves no place for the directory.

## Risks / Trade-offs

- [A busy team Chat floods the Unread view] → A Chat is one entry however many messages arrive, and it can be archived; a new message brings it back, as for Threads.
- [Keyset paging over two tables] → Both sides share the ordering key and are indexed on it. The page size stays 100.

## Acceptance (2026-11-01)

On the local stack, with the demo client on the debug build and peer1 as the other member:
- The team Chat "Acceptance team (renamed)" was filed in the folder "Project Falcon" and archived. It left All and the folder and showed in the Archive.
- peer1 posted "Falcon review moved to **Thursday**". The Chat left the Archive and showed first in Unread and in "Project Falcon", with a Chat badge, the message as preview and an unread pill of 1. It was also back in All.
- inbox.counts gave Unread 4: three Threads and the Chat. app.status gave messaging unread 1, which is the tray count, and the Chats navigation entry showed 1.
- The Chats entry showed the list filtered to Chats, with the directory and New team Chat in the reading pane.
- Opening the Chat from the folder showed the conversation in the reading pane with the folder list still on the left, and marked it read: Unread fell to 3 and the unread pill went away. That the user's own messages do not un-archive a Chat is covered by MessagingInbox.ChatsAreInboxEntriesWithTriage.
- Hidden-window UI tests cover rows, the context menu, the conversation, the directory, Leave Chat and policy.messaging.mode off. win_tests (Shell, Ui, Inbox, Messaging, Knowledge) passed 166 and ui_tests passed 56.
