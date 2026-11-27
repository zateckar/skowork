# Spec Delta

## ADDED Requirements

### Requirement: Sent Mail Event
The mail sync SHALL publish a "mail sent" event for each new, non-draft message that appears in Sent Items, except during the first sync of that folder (backfill) and for withheld messages. The event names the message; its conversation id and sent time are read from the local copy. It is published only to client/agenda and client/event-triage, which close work the user answered (close-replied-work); client/scheduler Triggers and client/knowledge-graph Sources do not receive it. Drafts and Outbox messages SHALL NOT produce it.

#### Scenario: Reply sent in Outlook
- **WHEN** the user replies to a mail in Outlook and the next sync finds the reply in Sent Items
- **THEN** one "mail sent" event names the reply, and no "mail received" event is published for it

#### Scenario: First sync
- **WHEN** Sent Items is synced for the first time
- **THEN** its existing messages publish no "mail sent" event
