# Design

## Context

The control plane already keeps a push connection per installation (WebSocket with an outbox and replay, control-plane/fleet-registry Push Delivery) and knows each installation's user and liveness. The client's control channel receives push messages and can send client messages over the same connection. Users are identified by user name (the token's preferred_username); display names come from the token's name claim at sign-in.

## Decisions

### D1. Relay through the control plane, store on the server
- **Choice:** messages are posted with REST (`POST /api/v1/messaging/chats/{id}/messages`, idempotent by client message id) and delivered as a push message of type `chat-message` to the other members' installations; a member fetches `GET .../messages?after=<seq>` to catch up. Each Chat has a sequence number per message. Tables: chats (id, kind, name, pair key for direct Chats), chat_members (chat, user, read_seq, joined_at), chat_messages (chat, seq, id, sender, text, at), presence (user, busy, updated_at), user_profiles (user, display name, last_seen).
- **Alternatives considered:** peer-to-peer (no reachability between laptops, no offline delivery); end-to-end encryption (key management and multi-device complexity; messages stay on the organization's control plane like other organization data; can be added later without changing the API shape).

### D2. Presence
- **Choice:** derived from push connections: the fleet registry tells the messaging module when a user's first connection opens and the last closes. Clients report `active` or `idle` through REST (`PUT /api/v1/messaging/presence`, with the installation id) on change, and set `busy` the same way. Changes push `presence-changed` to members of the user's Chats only, debounced to one per second per user.
- **Alternatives considered:** heartbeat-based presence (minutes of delay); broadcasting to everyone (traffic grows with the square of users).

### D3. Directory
- **Choice:** the users the control plane has seen (installations and user_profiles), minus the deny list and users whose last known effective policy has messaging off, searched by prefix of user name or display name, at most 50 results.
- **Alternatives considered:** querying the identity provider (the desktop client token cannot list users; a service account is not available).

### D4. Client
- **Choice:** a messaging service in the client keeps a local cache in the encrypted database (chats, last 500 messages per Chat, outbox), applies push messages, catches up on reconnect, and exposes RPCs to the shell. The Chats view is a top-level navigation entry next to Knowledge. The idle detection uses GetLastInputInfo, polled every 30 seconds. Tools are registered with the agent runtime like other tool groups.
- **Alternatives considered:** messages as Threads in the inbox (mixes agent work with person-to-person talk; the product owner wants a separate list).
- **Implementation notes (tasks 2.1-2.3):**
  - Cache: migration 1400 (the 1400-1499 range is user-messaging) adds msg_chats (the Chat as the server lists it, plus a dirty flag for a read position not yet reported), msg_messages (chat, id, seq, sender, text, at, and pending, queued_at, error for the outbox) and msg_state (own busy flag and the last reported Presence). The newest 500 messages per Chat are kept; a new Chat is fetched from last_seq - 500.
  - Sync: on every connect, and when a push shows a gap or an unknown Chat, the service lists the Chats, fetches `messages?after=` the newest cached seq per Chat, drops Chats no longer listed (unless they hold queued messages), reports dirty read positions, then sends the outbox.
  - Outbox: a message written offline is stored pending with a random client message id and sent once, in the order written, when connected; the server's idempotent id makes a retry after a lost reply harmless. A permanent refusal (too large, not a member) marks the message failed in the view; offline, timeouts, 429, 5xx and 401 leave it queued.
  - Read position: opening a Chat reports the last message as read; sending a message also moves the own read position, as the server does.
  - Own Presence: the app polls GetLastInputInfo every 30 s on the host tick and the service sends PUT /presence (active or idle, with X-Workbench-Installation) only when the state changes; busy is sent on reconnect while set.
  - Tools: messages.read and messages.send take a `chat` id or a `user`; messages.send with only `user` opens the direct Chat first. Each message in a tool result carries its own provenance (source kind message, the sender, untrusted).
  - Unread: the host status carries messaging.unread; the navigation entry shows the count, and the tray icon gets a red dot and a tooltip line while it is above zero.

### D5. Sizing
- **Choice:** 30k users with an average of 50 messages a day is 1.5 M messages a day, about 1 GB of text a day in SQLite at 600 bytes each; with 90 days retention about 90 GB. A deployment keeps the default retention shorter if disk is limited (server setting). Delivery is one push per connected installation per message.

## Risks / Trade-offs

- [Message content is personal data] → Retention deletes old messages; content is never in audit logs, telemetry or the console; access is limited to members.
- [Agent sends a message the user did not intend] → messages.send is an external-risk tool that needs approval by default; the approval shows recipient Chat and text.
- [A message contains instructions for the recipient's agent] → Message content is untrusted data with the sender as provenance.
- [Directory leaks who uses Workbench] → Only signed-in employees can query it, and it shows only names and Presence.

## API (implemented in tasks 1.1-1.2)

All under `/api/v1/messaging` with the user's access token; every call answers 403 MESSAGING_OFF when policy.messaging.mode is off.
- `GET /directory?q=` → `{users: [{user_name, display_name, presence}]}` (at most 50).
- `PUT /presence` `{state?: active|idle, busy?: bool}` (state needs the `X-Workbench-Installation` header) → `{presence, busy}`.
- `GET /chats` → `{chats: [{id, kind, name, members: [{user_name, display_name, presence}], last_seq, read_seq, unread, last_message, created_at}]}`, newest activity first.
- `POST /chats` `{kind: direct, user}` or `{kind: team, name, members: [...]}`; `PATCH /chats/{id}` `{name}`; `POST /chats/{id}/members` `{users}`; `DELETE /chats/{id}/members/me`.
- `GET /chats/{id}/messages?after=<seq>&limit=` → `{messages: [{id, chat, seq, sender, sender_name, text, at}]}`; `POST /chats/{id}/messages` `{id, text}` (id 8-64 of `[A-Za-z0-9_-]`, idempotent); `POST /chats/{id}/read` `{seq}`.
- Push types: `chat-message` `{chat, message}` to every member's installations (the sender's too; clients drop duplicates by id), `chat-changed` `{chat}`, `presence-changed` `{user_name, presence}` to the user's Chat peers.

## Acceptance (task 3.1, 2026-11-01)

Local stack (dev-stack.ps1), a development client signed in as the organization Keycloak user dev-user, and a second user peer1 ("Petra Peer") signed by a local test issuer and acting through the API and the push channel.
- Directory: each user finds the other with Presence online.
- Direct Chat: the client created it and sent Markdown; peer1 received the chat-message push and replied; the client showed both messages in order.
- Team Chat: peer1 created it with dev-user; the client showed it with 1 unread, replied, and renamed it; peer1 received the messages and chat-changed pushes.
- Presence: peer1 set busy and the client's directory showed busy; the client's busy toggle reached peer1 as presence-changed busy, then online.
- Offline catch-up: with the client quit, peer1 saw it offline and sent two messages; after restart the client showed 2 unread and all four messages in order, and peer1 saw it online again.
- The Chats view in the running window showed both Chats with last message and Presence.
- Not observed live: the idle switch to away after 10 minutes (covered by Messaging tests).
