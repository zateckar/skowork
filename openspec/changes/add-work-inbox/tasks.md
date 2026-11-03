# Tasks

## 1. Validation spike

- [ ] 1.1 Prototype the list with 10,000 synthetic Threads in WebView2 on the baseline laptop. Verify first paint under 300 ms and no dropped frames during scrolling

## 2. Thread model and views

- [x] 2.1 Add Thread inbox fields (unread, archived, pinned, folder, origin, origin link) and the derived has_action and running flags in `app.db`. Verify with unit tests for each state transition in the Read State and Archive requirements
- [x] 2.2 Implement view queries (Unread, Action, Running, All, Archive, folder) with keyset paging and count badges. Verify with query tests covering folder independence
- [x] 2.3 Implement folders (create, rename, delete, move) and pinning with date grouping. Verify that deleting a folder moves its Threads to the Inbox in a test

## 3. Layout and feed

- [x] 3.1 Build the three-pane layout with the responsive Activity rail drawer. Verify the 1180 px breakpoint in UI tests
- [x] 3.2 Build the streaming Thread feed with tool-call cards and reasoning blocks. Verify by replaying recorded Run event streams in UI tests
- [x] 3.3 Build the Activity rail with grouped subagent Runs showing runtime Run states and lazy transcripts. Verify that the transcript is fetched only on expand

## 4. Action items

- [x] 4.1 Build the Approval card (approve, edit, reject, respond-instead) with rationale and provenance display. Verify each decision path resumes the Run correctly against the runtime test harness
- [x] 4.2 Build the Question card for ask_user and client-raised Questions (fixed choices, no free text). Verify that answering resumes the Run and removes the Thread from the Action view
- [x] 4.3 Implement standalone Action items raised by client capabilities without a Run, with answers routed back to the raising capability. Verify with a test capability that raises Connect / Not now

## 5. Composer and Thread actions

- [x] 5.1 Build the composer with attachments, per-Thread drafts and `/` skill listing. Verify that drafts persist across a client restart
- [x] 5.2 Implement queue versus steer, the queued list and Stop. Verify against the runtime harness that queued messages are sent after the Run and that steering reaches the active Run
- [x] 5.3 Implement fork from message, origin links (fork, trigger, remote, meeting) and the system origin badge for client-created Threads. Verify the links open the correct target and that a standalone Action item Thread shows the system badge
- [x] 5.4 Implement error banners with the remedies for all six model-access error codes, including Compact Thread through the runtime's manual compaction. Verify each code with a fault-injection test

## 6. Notifications, search, shortcuts, status, settings

- [ ] 6.1 Implement toast notifications with AUMID, focus suppression, silent cancel and toggles. Verify by clicking a toast and confirming the deep link opens the correct Thread
- [x] 6.2 Implement the FTS5 search with diacritic folding and jump-to-message. Verify Czech and German queries with and without diacritics
- [x] 6.3 Implement the keyboard shortcut catalogue and help overlay. Verify each shortcut in UI tests
- [x] 6.4 Implement the status bar (control-channel state, policy version, MCP health, context ring with Compact now). Verify the 75% warning and the display of automatic compaction
- [x] 6.5 Implement the settings shell with all sections and the managed read-only rendering. Verify a policy-fixed setting cannot be changed

## 7. Localization and accessibility

- [x] 7.1 Externalize all strings and add cs, de and en bundles. Verify with a CI check that no hard-coded strings exist and every key is translated
- [ ] 7.2 Run the axe-core CI gate and a manual NVDA and Narrator pass. Verify zero critical axe findings and that Action item announcements work

## 8. Integration

- [ ] 8.1 End-to-end test: a user-started Run in a background Thread raises an Approval while the window is closed, then a toast appears, then the Approval card opens with provenance, then the Run completes and the Thread becomes unread. The Trigger-started variant is verified in add-scheduled-and-remote-tasks
- [x] 8.2 Run `openspec validate --all --strict` and fix any findings

## Workflow follow-up

- Archive this change with `openspec archive add-work-inbox --yes` after review, then check that the merged specs read coherently.
