# Spec Delta

## MODIFIED Requirements

### Requirement: Smart Views
The inbox SHALL provide the views Unread, Action, Running, All and Archive.
- **Unread:** Threads with unseen results, and Chats with unread messages.
- **Action:** Threads with at least one Action item, including Threads whose Run is interrupted.
- **Running:** Threads with a Run in state queued or running.
- **All:** every Inbox entry that is not archived, whether in the Inbox or a folder.
- **Archive:** archived Inbox entries.
Unread and Action show a count badge when the count is greater than zero.

#### Scenario: Thread needs approval
- **WHEN** a Run is interrupted for an Approval
- **THEN** its Thread appears in the Action view and the Action count increases by one

#### Scenario: Running view spans all Threads
- **WHEN** three Threads have active Runs, including Runs started by Triggers while the window was closed
- **THEN** the Running view lists all three Threads after the window opens

#### Scenario: Unread Chat
- **WHEN** a message arrives in a Chat the user does not have open
- **THEN** the Chat appears in the Unread view and the Unread count includes it

## ADDED Requirements

### Requirement: Chat Entries
The inbox SHALL list every Chat of client/user-messaging as an Inbox entry next to the Threads, ordered with them by last activity. A Chat entry shows the Chat name, the last message, the relative time, its unread count, the other user's Presence for a direct Chat, and a Chat badge. Opening it SHALL show the conversation with its composer in the reading pane. Chat entries never appear in Action or Running, and are not listed while policy.messaging.mode is off.

#### Scenario: Open a Chat entry
- **WHEN** the user opens the Chat entry of a direct Chat with bob from the Unread view
- **THEN** the reading pane shows the conversation and its composer, and the entry leaves Unread

### Requirement: Chat Entry Triage
The user SHALL be able to pin a Chat entry, move it to a folder or back to the Inbox, and archive and restore it, as for Threads. A new message in an archived Chat un-archives it, back to its folder or the Inbox. Chat entries cannot be deleted; leaving the Chat removes its entry.

#### Scenario: File a Chat in a folder
- **WHEN** the user moves the team Chat "Falcon team" to the folder "Project Falcon"
- **THEN** the folder lists the Chat next to its Threads, and the Chat still appears in Unread while it has unread messages

#### Scenario: Archived Chat comes back
- **WHEN** a colleague writes in a Chat the user archived
- **THEN** the Chat leaves the Archive and appears in All and Unread, in its folder or the Inbox
