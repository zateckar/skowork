# Spec Delta

## ADDED Requirements

### Requirement: Replied Items Close
An open Agenda item whose newest source is mail or a Teams message SHALL be closed as done, with a notice that the user replied, when the user replies to that source after it arrived. Its preparation Run SHALL be cancelled and its preparation Thread archived. Detection SHALL NOT add an item from a source that the user had already answered. A reply is as defined by client/event-triage Replied Items Close: for mail a sent, non-draft message in Sent Items with the same conversation id, for Teams the user's own message in the same conversation (in a channel, the same thread); subjects SHALL NOT be used to match. The user SHALL be able to reopen the item.

#### Scenario: Answered after the item was added
- **WHEN** an open item comes from a request mail received at 08:56 and the user replies in that conversation at 09:16
- **THEN** the item is done with the notice "You replied", and its preparation Thread is archived

#### Scenario: Answered before detection
- **WHEN** the user replied to a request mail before the Agenda evaluated it
- **THEN** no Agenda item is added for that request

#### Scenario: Answered in Teams
- **WHEN** an open item comes from a Teams chat message asking for the slides and the user answers in that chat
- **THEN** the item is done with the notice "You replied"

#### Scenario: Asked again after the reply
- **WHEN** the user replied at 09:16 and the sender asks a new question in the conversation at 10:00
- **THEN** the new request can become an open item
