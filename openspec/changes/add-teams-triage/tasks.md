## 1. Teams scope in event triage

- [x] 1.1 Write a unit test in `client/tests/mail/triage_test.cpp`: with Teams triage off nothing is taken in; on, a one-to-one message becomes a suggestion, a group message without a mention and the user's own message do not, starting work passes provenance teams, and a deletion withdraws a pending item. Verify: `win_tests --gtest_filter=EventTriage.*` passes.
- [x] 1.2 Implement the scope in `client/src/triage/triage.{h,cpp}`, feed Teams events and the Teams lookup in `client/src/app/app.cpp`, add migration 2400 in `client/src/storage/schema.cpp`. Verify: tests 1.1 pass.
- [x] 1.3 Add the Teams switch and the suggestion label in `client/src/shell/views_triage.cpp` with strings in en, cs and de. Verify: the client builds.
- [ ] 1.4 Acceptance on the user's laptop: turn on Teams chats and Teams in Settings > Triage, receive a direct Teams message. Verify: a suggestion Thread appears in the Inbox.
