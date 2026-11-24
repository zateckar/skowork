# Proposal

## Why

On 2026-11-24 the user asked why Teams chats only appear in Settings: mail, calendar changes, Skowork chats and meetings reach the Inbox, but Teams messages do not. Today client/teams-chats feeds the Teams copy to knowledge, the Agenda and the Daily report only. client/event-triage, which turns new mail and calendar changes into suggestion Threads in the Inbox, has no Teams scope.

## What Changes

- Event triage gets a third scope, Teams messages, next to mail folders and calendar events, with its own switch in Settings > Triage (off by default, like the calendar).
- Eligible: a message from someone else, not a system message, addressed to the user (a one-to-one chat or a mention of the user), in a conversation that is not excluded, while Teams chats are on.
- The "Teams message" events of client/teams-chats feed triage. A Teams deletion or exclusion withdraws the suggestion. The Teams backfill publishes no events, so history is not suggested.
- Suggestion Threads for Teams name the sender and the start of the message. Starting work passes the message as untrusted content with provenance teams.
- Storage: `triage_items.scope` accepts `teams` (schema migration 2400).

Out of scope:
- A Thread for every Teams conversation, replies to Teams (client/teams-chats Teams Is Read Only), and channel posts without a mention.

## Capabilities

### Modified Capabilities
- client/event-triage: Teams Messages Scope (added).
- client/teams-chats: Teams Event Feed (event triage is a consumer).
