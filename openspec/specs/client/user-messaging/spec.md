# client/user-messaging Specification

## Purpose
Defines messaging in the desktop app: the Chats view, the user's own Presence, offline sending and the messages.* tools.

## Requirements

### Requirement: Chats View
The client SHALL list Chats (control-plane/user-messaging) as Inbox entries (client/work-inbox Chat Entries) with unread count, last message and, for direct Chats, the other user's Presence. The Chats navigation entry shows only Chats and starts direct or team Chats from the user directory. An open Chat shows its messages (sender, time, Markdown) and a composer. The user can rename a team Chat, add members and leave it. The Chats entry and tray icon show the total unread count.

#### Scenario: Start a direct Chat
- **WHEN** the user searches the directory for bob, starts a Chat and sends "Hello"
- **THEN** the Chat appears in the list with bob's Presence, and bob's client shows it with one unread message

#### Scenario: Unread count
- **WHEN** a message arrives in a Chat the user does not have open
- **THEN** the Chat's unread count, the navigation entry and the tray icon show it, and opening the Chat clears it


### Requirement: Chat People Picker
The members of a new team Chat, and the members added to a team Chat, SHALL be chosen with a people picker, so no exact user name has to be typed:
- the user types part of a name or user name, and matching colleagues from the user directory are listed with their Presence;
- the user picks one or more of them; picked colleagues appear as chips that can be removed;
- colleagues who are already members are shown as members and cannot be picked again.

#### Scenario: Add members by searching
- **WHEN** the user chooses Add members in a team Chat, types "pet", picks Petra Peer from the matches and confirms
- **THEN** Petra Peer becomes a member of the Chat, and the user never typed her user name

#### Scenario: Existing member
- **WHEN** the picker lists a colleague who is already a member of the Chat
- **THEN** the colleague is marked as a member and cannot be picked
### Requirement: Own Presence
The client SHALL report the user as idle to the control plane after 10 minutes without keyboard or mouse input and as active when input resumes, and let the user set busy until they clear it. Messaging respects busy only for the display of Presence; messages are still delivered.

#### Scenario: Idle
- **WHEN** the user has not touched keyboard or mouse for 10 minutes
- **THEN** colleagues see the user as away

### Requirement: Offline Sending
The client SHALL keep the user's Chats and their recent messages in its encrypted local storage, show them while the control plane is unreachable, and queue messages written offline. Queued messages are sent in order when the push connection is back, and are shown as pending until the control plane accepts them.

#### Scenario: Written offline
- **WHEN** the user sends a message while disconnected and the connection returns
- **THEN** the message is sent once and stops being shown as pending

### Requirement: Messaging Tools
The client SHALL offer the agent the tools messages.find_user and messages.list_chats and messages.read (risk class read) and messages.send (risk class external). Message content returned to the agent is untrusted data with the sender as provenance under client/agent-runtime. Messages never start a Run. With policy.messaging.mode off, the tools are not offered.

#### Scenario: Agent drafts a reply
- **WHEN** the user asks the agent to answer bob's last message
- **THEN** the agent reads the Chat with messages.read and calls messages.send, which waits for the user's approval under the default policy for the external risk class
