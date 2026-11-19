## 1. Inbox store

- [x] 1.1 Migration 1410 with chat_inbox, kept current by the messaging service (D1); the merged list, views and counts with keyset paging, and triage of Chat entries (D2). Verify with inbox tests: a Chat in Unread, All, a folder and the Archive; un-archive on a new message; folder deletion; ordering and paging across both kinds; policy.messaging.mode off

## 2. Desktop app

- [x] 2.1 Chat rows with badge, Presence and unread pill, their context menu, the conversation in the reading pane (D3), and the Chats entry as the filtered list with the directory (D4); strings in en, de, cs. Verify with hidden-window UI tests and an a11y check

## 3. Acceptance

- [x] 3.1 On the local stack with the peer user: file a Chat in a folder, archive it, receive a message, and check Unread, the folder and the tray. Record the result in design.md
- [x] 3.2 Run `openspec validate --all --strict`
