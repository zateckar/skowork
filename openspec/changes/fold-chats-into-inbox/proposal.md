# Proposal

## Why

On 2026-11-02 the product owner asked whether the separate Chats entry in the navigation is still needed. Since add-chats-in-inbox, every Chat is an Inbox entry: Chats are listed, triaged, filed and counted with the Threads in Unread, All, Archive, the Inbox and folders. The Chats entry only remained as a filter of the list and as the place to start a Chat and to set busy. It is a second way into the same list, and it makes messaging look like a separate app.

## What Changes

- The navigation has no Chats entry and the list has no Chats-only view. Chats are found in the Inbox views like Threads.
- **New Chat** sits next to New thread at the top of the navigation. It opens a dialog with the people picker:
  - one colleague picked: a direct Chat (the existing one when there is one);
  - two or more: a team Chat, with a name.
  The new Chat opens in the reading pane.
- **Busy** moves to the status bar: the user's own Presence (Available or Busy) is shown there, and clicking it switches busy on or off.
- The unread count of Chats stays in the Unread count and on the tray icon.

Out of scope: changes to Chat entries, the conversation, notifications.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/user-messaging`: Chats View (modified), Own Presence (modified).

## Impact

- **Roadmap:** 27, after improve-window-frame. Depends on add-chats-in-inbox and improve-desktop-look (the people picker). It replaces add-chats-in-inbox D4 (the Chats entry).
- **Code:**
  - client/src/shell: the navigation without Chats, New Chat with its dialog, the busy switch in the status bar, the routes without the Chats-only view;
  - client/src/app: the user's busy state in app.status;
  - strings in en, de, cs.
- **Tests:** hidden-window UI tests of New Chat (direct and team), the status bar's busy switch and the navigation; updated golden images.
- **Data:** none.
