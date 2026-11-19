# Spec Delta

## MODIFIED Requirements

### Requirement: Chats View
The client SHALL list Chats (control-plane/user-messaging) as Inbox entries, shown as client/work-inbox Chat Entries describes. There is no Chats navigation entry; New Chat, next to New thread, starts a Chat with people from Chat People Picker: one makes a direct Chat, more a named team Chat. An open Chat shows its messages (sender, time, Markdown) and a composer; the user can rename a team Chat, add members and leave it. The Unread count and tray icon include unread Chats.

#### Scenario: Start a direct Chat
- **WHEN** the user chooses New Chat, picks bob, starts the Chat and sends "Hello"
- **THEN** the Chat opens in the reading pane and appears in the Inbox list with bob's Presence, and bob's client shows it with one unread message

#### Scenario: Start a team Chat
- **WHEN** the user chooses New Chat, picks bob and carol and names the Chat "Falcon team"
- **THEN** a team Chat "Falcon team" with the three of them opens in the reading pane

#### Scenario: Unread count
- **WHEN** a message arrives in a Chat the user does not have open
- **THEN** the Chat's unread count, the Unread count and the tray icon show it, and opening the Chat clears it

### Requirement: Chat People Picker
The members of a new Chat, and the members added to a team Chat, SHALL be chosen with a people picker, so no exact user name has to be typed:
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
The client SHALL report the user as idle to the control plane after 10 minutes without keyboard or mouse input and as active when input resumes, and let the user set busy until they clear it. The status bar shows the user's own Presence (Available or Busy) while messaging is on, and switches busy on or off when clicked. Messaging respects busy only for the display of Presence; messages are still delivered.

#### Scenario: Idle
- **WHEN** the user has not touched keyboard or mouse for 10 minutes
- **THEN** colleagues see the user as away

#### Scenario: Set busy
- **WHEN** the user clicks Available in the status bar
- **THEN** it shows Busy, and colleagues see the user as busy until the user clicks it again
