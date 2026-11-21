# Spec Delta

## MODIFIED Requirements

### Requirement: Agenda Is Opt In
The client SHALL detect Agenda items only after the user enables the Agenda and selects its sources (mail, calendar, meetings, Chats, Teams chats), and only while policy.agenda.mode is available. The Agenda SHALL be disabled by default.

#### Scenario: Not enabled
- **WHEN** the user has not enabled the Agenda
- **THEN** no source content is evaluated for Agenda items and the Agenda view is not shown

#### Scenario: Turned off by policy
- **WHEN** policy.agenda.mode is off
- **THEN** detection stops and the Agenda view is hidden, even if the user had enabled it

### Requirement: Agenda Item Sources
The client SHALL detect Agenda item candidates, arriving or changing after the Agenda was enabled, from: mail and calendar items of client/mail-calendar except those the user sent; transcripts of client/meeting-capture; direct Chats and team-Chat messages mentioning the user; and Teams messages of client/teams-chats, with prior messages as context. Older content SHALL NOT be scanned.

#### Scenario: Old mail is not scanned
- **WHEN** the user enables the Agenda with 3,000 messages already in the mailbox
- **THEN** none of those messages is sent for Agenda detection, and a message received afterwards is

#### Scenario: Request in a Teams chat
- **WHEN** after the Agenda was enabled a colleague writes to the user in Teams "can you send me the test report by Thursday?"
- **THEN** an Agenda item "Send the test report" due Thursday is created with the quote and a link to the Teams conversation

## ADDED Requirements

### Requirement: Sending A Prepared Draft
When a preparation produced a draft reply to a mail or Teams source, the client SHALL let the user edit it in a send form, prefilled with the reply's recipients, subject and the draft. Pressing Send with the complete message shown SHALL be the explicit approval of client/mail-calendar Send Always Requires Approval; the mail backend sends it as a reply. On the Local Outlook backend the user MAY open it in Outlook instead. No Run sends anything.

#### Scenario: Edit and send
- **WHEN** the user opens the draft reply of a prepared Agenda item, changes a sentence and presses Send
- **THEN** exactly the edited message is sent in reply to the source mail, and the Agenda item offers Mark done

#### Scenario: Open in Outlook
- **WHEN** the user chooses Open in Outlook on the Local Outlook backend
- **THEN** a reply draft with the edited text is saved in Outlook and shown in an Outlook window, and nothing is sent

#### Scenario: Reply to a Teams message
- **WHEN** the prepared item came from a Teams message
- **THEN** the form addresses the mail to the Teams sender's address, and the user can also copy the text to answer in Teams
