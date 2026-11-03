# client/knowledge-graph Specification

## Purpose
The user's local, private knowledge store on the laptop: what it ingests (Threads, files, mail, calendar, meetings and the user's notes), how it is indexed and searched with provenance, and how the user keeps notes in it, including notes saved from a mail item or a Chat. Knowledge leaves the laptop only as LLM context through the gateway, or when the user shares it with a Group.

## Requirements

### Requirement: Save As Note
The client SHALL let the user save an indexed mail item, or the messages of a Chat over a chosen period (the last 24 hours, the last 7 days, or everything kept on the computer), as a note under Knowledge Notes. It opens the note editor filled with the title, a header (sender, recipients and date, or participants and period) and the text, and stores nothing until the user saves. With policy.knowledge.save_as_note off (on or off, default on), no Save as note action is offered.

#### Scenario: Save a Chat
- **WHEN** the user chooses Save as note for a team Chat with the period Last 7 days
- **THEN** the editor holds the messages of the last 7 days with their senders and times, and nothing is stored until the user saves

#### Scenario: Switched off by policy
- **WHEN** policy.knowledge.save_as_note is off
- **THEN** neither mail items nor Chats offer Save as note

### Requirement: Saved Note Origin
A note saved from mail or a Chat SHALL be private by default, editable and shareable under Sharing Knowledge Items like any note. It records where it came from, and the Knowledge view shows that origin with a link to the mail item or Chat. Sharing such a note SHALL say in the confirmation that it contains copied mail or Chat content.

#### Scenario: Share a note saved from mail
- **WHEN** the user saves a mail item as a note and shares the note with a permitted Group
- **THEN** the note shows that it was saved from mail, and the share confirmation says it contains copied mail content
