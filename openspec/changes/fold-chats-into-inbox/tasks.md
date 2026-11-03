## 1. Desktop app

- [x] 1.1 Remove the Chats entry, the Chats-only view and the directory mode (D1). Verify with UI tests: no nav-chats, `#/chats` opens the Inbox, a Chat entry still opens its conversation
- [x] 1.2 New Chat button and dialog: direct with one person, team with two or more and a name (D2); strings in en, de, cs. Verify with hidden-window UI tests and an a11y check
- [x] 1.3 busy in app.status.messaging, and the Presence button in the status bar (D3). Verify with UI tests: the button's name, messaging.busy called with the new value

## 2. Acceptance

- [ ] 2.1 On the local stack with the peer user: start a direct Chat and a team Chat with New Chat, switch busy in the status bar and check the peer sees it; regenerate and review the golden images. Record the result in design.md
- [x] 2.2 Run `openspec validate --all --strict`
