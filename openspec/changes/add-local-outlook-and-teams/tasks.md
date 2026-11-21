# Tasks

## 1. Validation spikes (on an organization laptop; the development machine has no Outlook or Teams)

- [ ] 1.1 Run the Outlook backend against classic Outlook on an organization laptop: attach, list folders, read 1,000 messages, read the calendar with recurrences, read labels, free/busy, create a reply draft, send to a test address, and accept an invitation. Verify that results match Outlook, and record any object-model guard prompts and the time per 1,000 messages
- [ ] 1.2 Check the Jet date filter of the calendar view in cs, de and en-US locales. Verify that occurrences at window edges and across a DST change are included exactly once
- [ ] 1.3 Read the new Teams cache of a test account: conversations, messages, profiles and mentions. Measure a 200 MB cache. Verify there are no decode errors on current Teams, the read takes under 20 seconds with under 300 MB peak, and Teams keeps working during the read. Check the decoder assumptions listed in design D4 against the real cache: the IndexedDB key type bytes, the database-id coding, and the Teams field names

## 2. Local Outlook backend (client/mail-calendar)

- [x] 2.1 Add the local transport to GraphClient and policy.mail.backend with auto selection; switching backends clears the copy and backfills without events. Verify with unit tests for selection and the transport hook. Status: done 2026-11-21; `OutlookBackend.*` in win_tests
- [x] 2.2 Implement OutlookGraph: the routes of design D2 over an OutlookSession interface, delta emulation with snapshots and 410 on an unknown generation, occurrence ids, transaction-id idempotency, and Graph-shaped JSON with labels. Verify with unit tests against a fake session, including a MailSync cycle, a delta with changes and removals, and an expired generation. Status: done 2026-11-21; `OutlookGraph.*` and `OutlookBackend.*`
- [x] 2.3 Implement ComOutlookSession: the STA worker, GetActiveObject attach, retries on a busy Outlook, the Table-based listing, sender SMTP resolution, the msip_labels property, the Jet calendar restriction in the user's locale, free/busy, writes and send. Verify that it compiles and links; behaviour is verified in 1.1. Status: compiles and links under -Werror, 2026-11-21; never run against Outlook
- [x] 2.4 Show the backend and the Outlook state in the Mail and calendar settings section, with the "Allow Skowork to start Outlook" setting. Verify with a shell test. Status: done 2026-11-21; `ShellSettings.OutlookTeamsAndDailyReportSections`

## 3. Teams chats (client/teams-chats)

- [x] 3.1 Implement the Snappy decoder, the LevelDB reader (log, table, MANIFEST live set, sequence merge) and the snapshot copy with share flags. Verify with unit tests on generated logs and tables, including deletions and an obsolete table, and add a fuzz target for the decoders. Status: done 2026-11-21; teams_tests, fuzz_teams_cache (5,000,000 local runs, no crash)
- [x] 3.2 Implement the IndexedDB key and metadata decoding and the Blink/V8 value deserializer (including Snappy-wrapped values). Verify with unit tests on values produced by a test serializer, and add a fuzz target. Status: done 2026-11-21; teams_tests
- [x] 3.3 Implement the Teams extraction (conversations, reply chains, profiles, lenient fields), the Teams store with FTS, the sync with its window, deletions, exclusions and events, and policy.teams.*. Verify with unit tests on a generated cache: backfill without events, a new message, a mention, an edit, a delete, an excluded conversation. Status: done 2026-11-21; extraction in teams_tests, the service in `TeamsService.*` (win_tests) with a scripted extraction
- [x] 3.4 Implement teams.search and teams.read. Verify with unit tests. Status: done 2026-11-21; `TeamsService.ToolsSearchAndRead`
- [x] 3.5 Add the Teams chats settings section (enable, profile, counts, last read, exclusions, forget). Verify with a shell test. Status: done 2026-11-21; `ShellSettings.OutlookTeamsAndDailyReportSections`

## 4. Knowledge and Agenda

- [x] 4.1 Add the Source kind teams to knowledge (conversation-day items, re-index on change, removal on exclusion). Verify with a knowledge test. Status: done 2026-11-21; `KnowledgeTeams.ConversationDaysAreIndexedAndRemovedWithTheirConversation` (it found that the start-up scan skipped Teams)
- [x] 4.2 Offer Teams messages to the Agenda as kind teams, with context. Verify with an Agenda test. Status: done 2026-11-21; `AgendaIntake.ATeamsMessageCarriesItsConversationAsContext`, `AgendaService.TeamsMessagesAreASelectableSource`
- [x] 4.3 Add "Reply…" to the Agenda item: the prefilled send form, Send through the mail backend as a reply, Open in Outlook, and copying the text for Teams. Verify with unit tests for draft extraction and the prefill, and a mail test that sends through the fake backend. Status: done 2026-11-21; draft extraction `AgendaDraft.*`, the prefill `AgendaIntake.AReplyTo*`, the form `ShellAgenda.ReplyFormSendsTheEditedDraft`, and sending a reply through the fake Outlook `OutlookBackend.ToolsSearchReadAndReplyThroughOutlook`

## 5. Calendar page and Daily report

- [x] 5.1 Add the `calendar.view` RPC and the Calendar page (day, work week, agenda list, event details and actions, privacy masking). Verify with a shell test. Status: done 2026-11-21; `ShellCalendar.WorkWeekPrivacyAndEventActions` (the RPC itself is exercised only through the app)
- [x] 5.2 Implement the Daily report service: settings, policy.daily_report.mode, schedule with the late start, digest assembly with caps, the read-only Unattended run in a system-origin Thread, and the notification. Verify with unit tests for the schedule and the digest. Status: done 2026-11-21; report_tests (the app's collection and the read-only hook are not covered by a test)
- [x] 5.3 Add the Daily report settings section. Verify with a shell test. Status: done 2026-11-21; `ShellSettings.OutlookTeamsAndDailyReportSections`

## 6. Integration

- [ ] 6.1 Run `openspec validate --all --strict` and fix findings. Status: this change and add-mail-calendar validate strictly (2026-11-21); `--all` still fails on five other in-flight changes (add-api-connectors, improve-generated-surface-guidance, improve-proactive-durable-work, improve-skowork-and-knowledge-control, stabilize-agent-tool-catalog), which this change does not touch
- [ ] 6.2 (Needs 1.1 to 1.3) Run an end-to-end scenario on an organization laptop: a mail with a request and a Teams mention produce Agenda items with drafts, the next morning's Daily report lists them, and the user sends one edited draft through Outlook

## Workflow follow-up

- Archive with `openspec archive add-local-outlook-and-teams --yes` after the spikes and 6.2.
