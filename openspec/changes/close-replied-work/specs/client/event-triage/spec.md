# Spec Delta

## ADDED Requirements

### Requirement: Replied Items Close
A triage action item from mail or Teams that is open or started SHALL be closed as replied when the user replies to its source after the source arrived: the Thread's active Run SHALL be cancelled, the Thread archived, and the Thread SHALL say that the user replied. Triage SHALL NOT create a Thread for a source that the user had already answered. For mail, a reply is a sent, non-draft message in Sent Items with the same conversation id. For Teams, a reply is a non-system message of the user in the same conversation posted after the source; in a channel it SHALL be in the same thread. Later revisions of the same source SHALL NOT reopen a replied item, and it SHALL no longer be listed as an open action item in the Daily report.

#### Scenario: Replied in Outlook
- **WHEN** a prepared action Thread waits on its Question card and the user answers the mail in Outlook
- **THEN** the Run is cancelled, the Thread moves to the Archive, and restoring it shows that the user already replied

#### Scenario: Replied in Teams
- **WHEN** a colleague asks the user a question in a one-to-one Teams chat, triage opens an action Thread, and the user answers in that chat
- **THEN** the Thread is archived and shows that the user already replied

#### Scenario: Another thread of the channel
- **WHEN** the user is mentioned in one channel thread and later posts in a different thread of that channel
- **THEN** the action Thread for the mention stays open

#### Scenario: Replied before triage
- **WHEN** the user answered a mail before triage classified it as needing action
- **THEN** no action Thread is created for it
