# Spec Delta

## ADDED Requirements

### Requirement: Teams Messages Scope
Event triage SHALL offer Teams messages as a scope beside mail folders and calendar events, switched on and off in Settings > Triage (default off). A Teams message is eligible while Teams chats are on (client/teams-chats) and it is from someone other than the user, not a system message, not empty, in a conversation that is not excluded, and addressed to the user: in a one-to-one chat or mentioning the user. "Teams message" and "Teams message updated" events SHALL be taken in like new mail; "Teams message deleted" and exclusion SHALL withdraw the item. A suggestion Thread for a Teams message SHALL be titled with the sender and the start of the message, and starting work SHALL pass the message as untrusted content with provenance teams.

#### Scenario: Direct message
- **WHEN** Teams triage is on and a colleague writes "Please review the budget by Friday" in a one-to-one chat
- **THEN** the message is classified like new mail, and when relevant a Thread "<colleague>: Please review the budget by Friday" appears in the Inbox

#### Scenario: Group chat without a mention
- **WHEN** a colleague writes in a group chat without mentioning the user
- **THEN** the message is not taken in by triage

#### Scenario: Message deleted in Teams
- **WHEN** a message with an open suggestion is deleted in Teams
- **THEN** the suggestion is withdrawn
