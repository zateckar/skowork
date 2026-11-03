# Spec Delta

## Purpose

Defines how the control plane relays messages between users: the user directory, Presence, Chats and their messages, and retention.

## ADDED Requirements

### Requirement: User Directory
The control plane SHALL list the users who have signed in to Workbench at least once and whose effective policy.messaging.mode is on, with user name, display name and Presence, searchable by name. Users on the deny list MUST NOT be listed. policy.messaging.mode takes the values on and off, default on.

#### Scenario: Find a colleague
- **WHEN** alice searches the directory for "bob"
- **THEN** the result lists bob with his display name and Presence

#### Scenario: Messaging off
- **WHEN** carol's Group policy sets policy.messaging.mode to off
- **THEN** carol is not listed, and her requests to the messaging API are refused with 403

### Requirement: Presence
The control plane SHALL keep each user's Presence as one of online, away, busy and offline. A user is offline while none of their installations has an open push connection, busy while they have set busy, away while every connected installation reports the user idle, and online otherwise. Presence changes are sent to the members of the user's Chats within 5 seconds.

#### Scenario: Going offline
- **WHEN** bob closes Workbench on his only laptop
- **THEN** alice, who shares a Chat with bob, sees bob as offline within 5 seconds of the push connection closing

### Requirement: Chats
The control plane SHALL keep two kinds of Chat: a direct Chat between exactly two users, one per pair, and a team Chat with a name and 2 to 50 members. Any member of a team Chat can rename it and add members, and any member can leave it. Only members can read a Chat or post to it. Creating a Chat, renaming it and membership changes are recorded in the audit log without message content.

#### Scenario: Direct Chat is unique
- **WHEN** alice starts a direct Chat with bob twice
- **THEN** both requests return the same Chat

#### Scenario: Not a member
- **WHEN** carol requests the messages of a Chat she is not a member of
- **THEN** the request is refused with 404

### Requirement: Message Relay
The control plane SHALL accept a text message of up to 8 KB from a Chat member, store it with sender, Chat and time, and deliver it through the push connection to every connected installation of every other member. Each member can fetch the messages after a position, so installations that were offline catch up. A client-chosen message id makes a repeated send of the same message idempotent. Each member's read position is kept for unread counts and is not shown to other members.

#### Scenario: Offline member catches up
- **WHEN** alice sends a message to a team Chat while bob is offline, and bob connects later
- **THEN** bob's client fetches the message after its last position and shows it once

#### Scenario: Repeated send
- **WHEN** a client resends a message with the same message id after a lost response
- **THEN** the message is stored and delivered once

### Requirement: Message Retention
The control plane SHALL delete messages older than the retention period, a server setting with default 90 days, and Chats with no members. Message content MUST NOT appear in the audit log, telemetry or the admin console; the admin console shows only counts of Chats and messages.

#### Scenario: Retention
- **WHEN** a message is older than the retention period
- **THEN** it is deleted and no member can fetch it
