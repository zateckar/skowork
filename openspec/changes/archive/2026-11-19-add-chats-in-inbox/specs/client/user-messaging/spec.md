# Spec Delta

## MODIFIED Requirements

### Requirement: Chats View
The client SHALL list the Chats of control-plane/user-messaging as Inbox entries (client/work-inbox Chat Entries), each with its unread count, last message and, for direct Chats, the other user's Presence. The Chats entry of the navigation SHALL show the Inbox list filtered to Chats, with the user directory to start a direct Chat or a team Chat. Opening a Chat shows its messages with sender and time, rendered as Markdown, and a composer. The user can rename a team Chat, add members and leave it. The Chats navigation entry and the tray icon show the total unread count.

#### Scenario: Start a direct Chat
- **WHEN** the user searches the directory for bob, starts a Chat and sends "Hello"
- **THEN** the Chat appears in the list with bob's Presence, and bob's client shows it with one unread message

#### Scenario: Unread count
- **WHEN** a message arrives in a Chat the user does not have open
- **THEN** the Chat's unread count, the navigation entry and the tray icon show it, and opening the Chat clears it
