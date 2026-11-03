# Spec Delta

## MODIFIED Requirements

### Requirement: Chats View
The client SHALL offer a Chats view in its navigation, showing the Chats of control-plane/user-messaging with the unread count, the last message and, for direct Chats, the other user's Presence. Opening a Chat shows its messages with sender and time, rendered as Markdown, and a composer. The user can start a direct Chat or a team Chat from the user directory, rename a team Chat, add members and leave it. The navigation entry and the tray icon show the total unread count.

The members of a new team Chat, and the members added to a team Chat, are chosen with a people picker:
- the user types part of a name or user name, and matching colleagues from the user directory are listed with their Presence;
- the user picks one or more of them; picked colleagues appear as chips that can be removed;
- colleagues who are already members are shown as members and cannot be picked again.

No exact user name has to be typed.

#### Scenario: Start a direct Chat
- **WHEN** the user searches the directory for bob, starts a Chat and sends "Hello"
- **THEN** the Chat appears in the list with bob's Presence, and bob's client shows it with one unread message

#### Scenario: Unread count
- **WHEN** a message arrives in a Chat the user does not have open
- **THEN** the Chat's unread count, the navigation entry and the tray icon show it, and opening the Chat clears it

#### Scenario: Add members by searching
- **WHEN** the user chooses Add members in a team Chat, types "pet", picks Petra Peer from the matches and confirms
- **THEN** Petra Peer becomes a member of the Chat, and the user never typed her user name

#### Scenario: Existing member
- **WHEN** the picker lists a colleague who is already a member of the Chat
- **THEN** the colleague is marked as a member and cannot be picked
