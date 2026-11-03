# Proposal

## Why

On 2026-11-01 the product owner asked whether Chats can be part of the Inbox with its views and folders. Today Chats live in their own view (client/user-messaging Chats View). A user who files work by project has the agent's Threads in folders and the conversations with colleagues somewhere else. Unread work is also split: the Unread view counts Threads, and the Chats entry counts messages.

The product owner chose Chats as Inbox entries.

## What Changes

- Every Chat becomes an Inbox entry next to the Threads. The entry shows:
  - the Chat name and the last message;
  - the time;
  - the unread count;
  - the other user's Presence for a direct Chat;
  - a Chat badge.
- Chats appear in Unread while they have unread messages, and in All, Archive, the Inbox and folders like Threads. They never appear in Action or Running.
- The user can pin a Chat, move it to a folder and archive it. A new message brings an archived Chat back.
- Opening a Chat entry shows the conversation in the reading pane, with its composer and its header actions (rename, add members, leave, busy).
- The Unread count and the tray include Chats with unread messages.
- The Chats navigation entry stays. It shows the Inbox list filtered to Chats, with the directory to start a direct Chat or a team Chat.

Out of scope:
- Full-text search of Chat messages in the Inbox search, which stays Threads only.
- Notifications for Chat messages, which are unchanged.
- Read state is not set by hand: a Chat is read when it is opened, as today.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/work-inbox`:
  - Smart Views (modified);
  - Chat Entries (added).
- `client/user-messaging`: Chats View (modified).

## Impact

- **Roadmap:** 23, after add-save-as-note. Depends on add-work-inbox, add-native-ui and add-user-messaging.
- **Glossary:** "Inbox entry" is added (a Thread or a Chat as listed in the Inbox views), and Inbox and Inbox views refer to entries.
- **Code:**
  - client/src/inbox: Chat entries with folder, pin and archive state, and the merged list and counts;
  - client/src/messaging: activity and unread changes reported to the inbox;
  - client/src/shell: list rows of Chats, the reading pane hosting the conversation, and the Chats filter;
  - strings in en, de, cs.
- **Data:** an app.db migration adds a table for the Inbox state of Chats.
