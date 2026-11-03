## 1. Control plane

- [x] 1.1 Add the messaging tables, Chats, membership, message relay with idempotent ids and catch-up, read positions, retention and audit (D1). Verify with tests: direct Chat uniqueness, membership checks, idempotent send, catch-up after offline, retention, no content in audit
- [x] 1.2 Add Presence from push connections and client reports, and the directory (D2, D3), and policy.messaging.mode. Verify with tests: online, away, busy, offline within 5 s, directory search, messaging off refused
- [x] 1.3 Show Chat and message counts in the admin console dashboards. Verify in the browser

## 2. Client

- [x] 2.1 Messaging service: local cache, push handling, catch-up, outbox, own Presence (D4). Verify with tests against an in-process control plane fake
- [x] 2.2 Chats view: list, conversation, composer, directory, team Chats, unread counts in navigation and tray, strings in en, de, cs. Verify with hidden-window UI tests and the a11y check
- [x] 2.3 Tools messages.find_user, messages.list_chats, messages.read, messages.send. Verify with runtime tests: untrusted provenance, send waits for approval

## 3. Acceptance

- [x] 3.1 On the local stack, two users exchange messages in a direct and a team Chat, one goes offline and catches up, Presence changes are seen. Record the result in design.md
- [x] 3.2 Run `openspec validate --all --strict`
