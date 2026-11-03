# Tasks

## 1. Validation spikes

- [ ] 1.1 Acquire a token silently via WAM from a C++ test app built with GCC/MinGW (WinRT ABI) on a standard organization laptop, using the organization app registration. Verify there is no prompt, and record the device join type and any conditional-access blocks
- [ ] 1.2 Read sensitivity labels of messages and events through Graph in the test tenant. Verify that label GUIDs are returned for each labelled fixture item
- [ ] 1.3 Measure the initial sync of a 20k-message test mailbox on the baseline laptop. Verify it completes within 30 minutes at under 5% average CPU

## 2. Graph mock (dev/graph-mock)

- [x] 2.1 Scaffold the Node.js and TypeScript service (Express) with the mock OAuth endpoints (discovery, authorize with auto-consent and PKCE, token with authorization_code and refresh_token grants, mock-only signing key) and a Graph auth check. Verify with tests that a full code+PKCE flow succeeds, a wrong verifier gets invalid_grant, and requests without a mock token are rejected
- [x] 2.1a Add OAuth fault injection (interaction_required, consent_required, invalid_grant, expired tokens) to the control API. Verify with tests that each injected fault is returned exactly as configured
- [x] 2.2 Implement the fixture loader with seeded deterministic ids and cs/de/en fixtures (labels, private items, recurring events, attachments). Verify that two runs with the same seed return identical data
- [x] 2.3 Implement the mail endpoints (mailbox settings, folders, list/get with OData subset, search, move, delete, attachments, sensitivity-label properties, delta, drafts, send capture). Verify with mock unit tests
- [x] 2.4 Implement the calendar endpoints (events, calendarView, delta, getSchedule, create, respond). Verify with mock unit tests
- [x] 2.5 Implement fault injection and the `/_mock` control API (reset, deliver mail, change event, captured sends, request log). Verify with tests that inject throttling and delta expiry
- [ ] 2.6 Add contract tests against sanitised recordings from the test tenant. Verify that a deliberately removed property fails the suite
- [ ] 2.7 Package as a single-file executable (Node.js single executable application) and a container image. Verify it starts in under 5 seconds in CI, and document usage in the mock's own help output

## 3. Client Graph access (client/mail-calendar)

- [ ] 3.1 Implement the OAuth token provider: policy.mail.oauth configuration, silent WAM acquisition, the interactive sign-in in the system browser with a loopback redirect, encrypted refresh-token storage, Disconnect mailbox, scope selection by policy, and the "Connect mailbox" standalone Action item in a system-origin Thread. Verify against the mock: full sign-in, refresh after restart, interaction_required raises the Question, the loopback listener exists only while a sign-in is pending and closes after the redirect or 5 minutes (socket check in the test)
- [x] 3.1a Implement policy.mail.auth = oauth|mock, with mock mode compiled out of release builds. Verify that the release build rejects mock auth and that a missing policy.mail.oauth disables mail features
- [x] 3.2 Implement the Graph HTTP layer: base URL from policy, validated against the compiled-in Graph host set in release builds; that host set registered as a platform connection exempt from egress rules; paging; Retry-After; backoff. Verify against the mock with injected faults, that a release build rejects a non-Graph host, and that sync works with an empty egress allowlist
- [ ] 3.3 Fuzz the Graph JSON response parsers. Verify the fuzz target runs in CI without crashes

## 4. Mail and calendar tools

- [x] 4.1 Implement mail.search and mail.read with label and exclusion filtering (policy.mail.exclusions fields), "withheld" placeholders, and attachment text through the core-tools extraction worker. Verify against labelled fixtures and a pdf/docx attachment fixture
- [x] 4.2 Implement mail.draft, mail.send (approval decision floor), mail.move and mail.delete with declared risk classes. Verify against the mock, with sends captured, that the interactive and Unattended approval scenarios pass and that an explicit policy.unattended.rules allow is the only path that sends without approval
- [x] 4.3 Implement calendar.list, calendar.get, calendar.find_free_time, calendar.create and calendar.respond. Verify the scenarios against mock fixtures, including private-item masking

## 5. Sync and event feed

- [x] 5.1 Implement the delta sync store for mail and calendar: the sync window, unlabelled-item handling, full resync on token expiry, no duplicate events, and desktop-host deferral. Verify with the mock's delta-expiry fault and labelled/unlabelled fixtures
- [x] 5.2 Publish normalised events (event created, updated and cancelled carry start and end times) to the event bus for client/scheduler and client/knowledge-graph. Verify with an integration test that delivers mail and reschedules an event through the control API and observes the events
- [x] 5.3 Implement offline search from the local store with staleness labelling. Verify by stopping the mock mid-test

## 6. Settings and integration

- [x] 6.1 Add the Mail and Calendar section to settings (connection status, user exclusions, last sync). Verify with UI tests against the mock
- [x] 6.2 Run an end-to-end scenario against the mock: the user asks to reply to a thread and book a meeting; draft, approval, send capture and invitation capture are all verified
- [x] 6.3 Run `openspec validate --all --strict` and fix findings
- [ ] 6.4 (Later, before any pilot; needs the app registration's client id, tenant id and registered redirect URI) Run the OAuth flow and a read-only mail and calendar smoke test against the real organization tenant with a test account. Verify that silent and interactive sign-in both work and that no Graph call outside the delegated scopes is made

## Workflow follow-up

- Archive with `openspec archive add-mail-calendar --yes` after review.
