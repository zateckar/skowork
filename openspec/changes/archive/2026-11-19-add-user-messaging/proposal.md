# Proposal

## Why

On 2026-10-31 the product owner asked for messaging between users, with the control plane as the gateway and a list of users with their availability. People who work with Skowork want to reach a colleague, or a few, without leaving the app, and the agent should be able to read and draft messages for them. The product owner chose direct and group conversations with agent access.

## What Changes

- The control plane relays and stores messages between users, keeps each user's Presence and offers a user directory (control-plane/user-messaging).
- Two kinds of Chat: a direct Chat between two people and a team Chat of up to 50 people that any member can name and extend.
- The desktop app gets a Chats view: the Chat list with unread counts and Presence, the conversation, a composer, starting direct and team Chats from the directory, and the user's own Presence (automatic, or busy set by hand).
- Messages arrive through the existing push connection; messages written offline are sent on reconnect.
- The agent gets tools to find users, list Chats, read messages and send a message; sending asks for approval by default.
- Policy key policy.messaging.mode (on | off) turns messaging on or off per Group.

Out of scope:
- Attachments, reactions, message editing and deletion, read receipts shown to others, calls and video.
- Messages starting Runs (no messaging Trigger kind).
- Messaging users outside the organization or across control planes.
- Desktop toast notifications (notifications stay off as decided earlier); unread counts appear in the app and the tray.
- Administrator access to message content.

## Capabilities

### New Capabilities
- `control-plane/user-messaging`: user directory, Presence, Chats, message relay and storage, retention.
- `client/user-messaging`: Chats view, own Presence, offline queue, the messages.* tools.

### Modified Capabilities
- `control-plane/fleet-registry`: Push Message Types (adds chat-message, chat-changed and presence-changed).

## Impact

- **Roadmap:** 18, after hide-work-details. Depends on add-client-foundation (control channel, push), add-policy-and-sandbox (policy), add-work-inbox and add-native-ui (UI).
- **Code:** control-plane/src/messaging (store, routes, push), control-plane/src/policy/keys.ts; client/src/messaging (service, store, tools), client/src/shell (Chats view), strings in en, de, cs.
- **Data:** chats, members and messages in the control-plane database with a retention period; a local cache in the client's encrypted database.
- **Glossary:** adds Chat and Presence; adds messages.* to the tool ids and messaging to the Policy bundle keys.
