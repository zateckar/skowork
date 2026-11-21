# Spec Delta

## ADDED Requirements

### Requirement: Teams Chat Source
The client SHALL offer the Source kind teams, which indexes the local Teams copy of client/teams-chats. Each knowledge item is one conversation day: the messages of one conversation on one local date. It is indexed with the same exclusions, entity and relation extraction, salience and Common-Sense Fact Triage as the Source mail, and its Passages carry the provenance teams. The Source is enabled like any other Source, and is unavailable while client/teams-chats is not enabled.

#### Scenario: Decision in a chat
- **WHEN** a colleague writes in a Teams chat "we go with supplier B, contract signed today"
- **THEN** knowledge.search for "supplier B" returns a Passage from that conversation day with a Citation that names the chat and the date

#### Scenario: Excluded conversation
- **WHEN** the user excludes a Teams conversation in client/teams-chats
- **THEN** its knowledge items and Passages are removed and its facts are not stored again
