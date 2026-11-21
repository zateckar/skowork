# Spec Delta

## Purpose

Defines how the client reads the user's Microsoft Teams chats from the local new Teams client, keeps a local copy, and makes it available to the agent, knowledge, the Agenda and the daily report.

## ADDED Requirements

### Requirement: Local Teams Cache Source
The client SHALL read Teams chats only from the cache that the new Microsoft Teams client keeps in the signed-in user's profile. It MUST NOT write to, lock or modify that cache, MUST NOT contact Teams or Microsoft services for chats, and MUST NOT read the cache while policy.teams.mode is off (values on and off; default on).

#### Scenario: Teams running
- **WHEN** Teams is running and writing to its cache while the client reads it
- **THEN** Teams is not blocked and keeps working, and the client reads a consistent copy

#### Scenario: Turned off by policy
- **WHEN** policy.teams.mode is off
- **THEN** the Teams chats settings section says the organization turned it off, nothing is read, and the Teams tools are unavailable

### Requirement: Teams Chats Are Opt In
The client SHALL read the Teams cache only after the user enables Teams chats in settings, and SHALL let the user pick the Teams profile when there are several. Disabling SHALL stop reading. Forgetting Teams chats SHALL delete the local Teams copy and everything derived from it, following client/local-storage.

#### Scenario: Enable
- **WHEN** the user enables Teams chats and one Teams profile exists
- **THEN** the client reads that profile's chats, and the settings section shows the number of conversations and messages and the time of the last read

### Requirement: Teams Sync
The client SHALL read the cache at policy.teams.sync_interval_minutes (default 10) as background work that follows the deferral rules of client/desktop-host. It keeps the conversations, and the messages within policy.teams.sync_days (default 30), in a local copy encrypted per client/local-storage, as plain text. A message Teams marks deleted SHALL be removed from the local copy. A message that only left Teams' cache SHALL stay until it leaves the sync window.

#### Scenario: New message
- **WHEN** a colleague writes in a chat and Teams has stored the message
- **THEN** the message is in the local copy after the next read

#### Scenario: Unrecognised cache
- **WHEN** a read finds a non-empty cache but no conversation it can decode
- **THEN** the local copy is unchanged and the settings section reports that the Teams data was not recognised

### Requirement: Teams Exclusions
The client SHALL let the user exclude conversations, and SHALL exclude every conversation whose type policy.teams.excluded_types lists (values chat, group, meeting, channel; default empty). Excluded conversations are not stored, indexed, sent to a model or returned by tools.

#### Scenario: Excluded chat
- **WHEN** the user excludes a private chat
- **THEN** its messages are removed from the local copy and from knowledge, and teams.search no longer finds them

### Requirement: Teams Tools
The client SHALL provide the tools teams.search (text, conversation, sender, date range) and teams.read (a message with the messages around it in its conversation, or a whole conversation within a date range), both of risk class read. Their results are untrusted content with provenance teams, as owned by client/agent-runtime.

#### Scenario: Search chats
- **WHEN** the agent calls teams.search for "cost estimate"
- **THEN** matching messages are returned with conversation, sender, time and text, most recent first

### Requirement: Teams Event Feed
The client SHALL publish "Teams message", "Teams message updated" and "Teams message deleted" events for messages in conversations that are not excluded, for the Source teams of client/knowledge-graph, for client/agenda and for client/daily-report. A "Teams message" event SHALL say whether the message is addressed to the user (a one-to-one chat, or a mention of the user). The first read after enabling SHALL publish no events.

#### Scenario: Mention in a group chat
- **WHEN** a colleague mentions the user in a group chat
- **THEN** a "Teams message" event that is marked as addressed to the user is published once

### Requirement: Teams Is Read Only
The client SHALL NOT send Teams messages, reactions or read receipts, and SHALL NOT change presence. A reply to a Teams message is prepared as mail or copied by the user (client/agenda Sending A Prepared Draft).

#### Scenario: Agent asked to reply in Teams
- **WHEN** the user asks Skowork to answer a Teams message in Teams
- **THEN** no Tool can post to Teams, and the agent offers a draft the user can copy or send as mail
