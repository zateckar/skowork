## 1. Knowledge

- [x] 1.1 Note origin (kg.db migration), knowledge.note.draft_from_mail and knowledge.note.draft_from_chat with the text formats of D2 and the size cap, and saving with origin (D1). Verify with knowledge tests: drafts from a fixture mail item and a cached Chat, period filtering, cap, origin kept on edit
- [x] 1.2 Add policy.knowledge.save_as_note to the control-plane key registry and regenerate the schema fixture (D5). Verify with the policy tests and a client test that the draft RPCs refuse when it is off

## 2. Desktop app

- [x] 2.1 Save as note in the mail reader and the open Chat with the period dialog (D3), the origin line with its link, and the extra share-confirmation line (D4); strings in en, de, cs. Verify with hidden-window UI tests

## 3. Acceptance

- [x] 3.1 On the local stack: save a mail item from the mock mailbox and a Chat as notes, share one with the acceptance Group, and find it as the peer user with knowledge.search. Record the result in design.md
- [x] 3.2 Run `openspec validate --all --strict`
