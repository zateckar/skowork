## 1. Validation Spikes

- [x] 1.1 Build a labelled corpus of at least 150 mail, calendar, meeting follow-up and Chat samples in en, cs and de (dev/graph-mock fixtures, no real mailbox), including newsletters, FYI mail, lunch chatter, broadcasts, past and vague dates. Run the extended classifier prompt against the real gateway; verify precision of admitted Agenda items is at least 0.9 and recall at least 0.7, and record the figures in design.md.
- [x] 1.2 Verify local resolution of relative dates and time zones ("by Friday", "next week", "tomorrow morning", cs and de phrasing) against a fixed table of source timestamps; record failures as dropped, not guessed.
  Status: done 2026-11-19; 30-row table in AgendaDueDates.RelativePhrasesAgainstAFixedTable. Three failures were fixed: German "Morgen" as morning, day-after-tomorrow phrases, and the offset at the due moment across a daylight-saving change. Details are in design.md, Date Resolution Check.
- [x] 1.3 Measure detection request size and latency per batch of 10 items against the real gateway; verify the 60-second interval keeps a busy day within the background model budget.

## 2. Detection And Admission

- [x] 2.1 Implement the Agenda's mail and calendar intake and the bounded detection request and candidate parser (design D1); verify with parser unit tests, intake unit tests (baseline, sent items, excluded folders, queue cap, backoff) and a fuzz target for the candidate schema.
  Status: done 2026-11-17; AgendaParse.* (agenda_tests), AgendaService.IsOffByDefaultAndScansNothingBeforeEnablement, AFailedOrMalformedDetectionKeepsTheItemsForLater, TheQueueKeepsTheNewestFiveHundredAndCountsTheRest. The app's mail and calendar adapters (sent folders are never received, backfill skipped) have no isolated test. fuzz_agenda_detection_smoke (50,000 mutated runs) passed on 2026-11-19.
- [x] 2.2 Implement the admission rules (design D2): quote check, responsibility, action, future date, confidence; verify each rule with unit tests, including a 40-recipient broadcast without the user's name and a past date.
  Status: done 2026-11-17; AgendaAdmit.* (agenda_tests) and AgendaService.NoiseLowConfidenceAndPastDatesAreDropped.
- [x] 2.3 Queue meeting transcripts when they are indexed, admitted only when a task names the user (design D1, changed 2026-11-17 from structured follow-ups); verify with meeting-service tests.
  Status: done 2026-11-19; the adapter moved to `client/src/app/agenda_intake.cpp`. AgendaIntake.AMeetingBecomesASpeakerLabelledItemLinkedToItsThread, AMeetingWithoutTranscriptTextIsNotOffered, ALongTranscriptKeepsItsStartAndEndOnCharacterBoundaries and AgendaService.MeetingTasksAreAdmittedOnlyWhenTheyNameTheUserAndLeaveWithTheMeeting.
- [x] 2.4 Add Chat intake for direct Chats and mentions (design D1); verify batching, interval and that history before enablement is not scanned.
  Status: done 2026-11-19; AgendaIntake.ADirectChatAddressesTheUserAndATeamChatDoesNot, TheUsersOwnAndEmptyChatMessagesAreHandled and AgendaService.ChatMessagesAreBatchedAtMostOncePerIntervalFromEnablementOn (10 then 2 items, one request per 60 seconds, a message from before enablement not queued).

## 3. Merging And Lifecycle

- [x] 3.1 Implement merging (design D3); verify source-identity merges, model-decided merges with an in-test fake, and that the earlier due date wins.
  Status: done 2026-11-17; DetectsAdmitsAndMergesWithOneSmallToolFreeRequest, TheSmallModelDecidesMergesAcrossConversations, AgendaTitles.* (agenda_tests).
- [x] 3.2 Implement states, user edits, dismissal suppression, withdrawal on revocation and retention (design D4); verify with storage tests including the local-storage deletion cascade.
  Status: done 2026-11-17; DismissedCommitmentsAreNotAddedAgainAndWithdrawalRemovesItems, UserEditsWinOverLaterSourceRevisions, ClosedItemsAreDeletedNinetyDaysAfterTheirStateChanged, DeleteAllRemovesItemsQueueAndCounts; Forget of the agenda Source is registered in the forget cascade.

## 4. Preparation

- [x] 4.1 Implement Prepare as a top-level Run limited to read-risk Tools with brief and draft instructions (design D5); verify that a write or external Tool is unavailable and that source content is tagged untrusted.
  Status: done 2026-11-19; PreparationStartsOnceWithAReadOnlyPreambleAndUntrustedData covers the service, and AgendaIntake.PreparationRunsAndTheirChildRunsMayOnlyRead the hook's rule (`agenda_preparation_decision`): write-local, external and destructive calls are denied in a preparation Run and its Child Runs, other Runs are unaffected.
- [x] 4.2 Implement automatic preparation with its opt-in, daily cap, 7-day window and single refresh; verify with scheduler-style unit tests and with an unattended policy that denies read Tools.
  Status: done 2026-11-19; AutomaticPreparationNeedsPolicyAndTheUsersChoice, AutomaticPreparationIsRefreshedOnceTheWorkingDayBeforeAfterASourceChanged and AutomaticPreparationNeedsUnattendedRunsThatMayRead. The service now starts no automatic preparation when policy.unattended.allowed_risk leaves out read (design D5).
- [x] 4.3 Run one local e2e preparation against the real gateway with Graph-mock data; verify the brief has Citations and the draft is labelled.

## 5. Inbox View And Reminders

- [x] 5.1 Add the Agenda view with grouping and row actions (design D6); verify with shell tests and UI Automation names.
  Status: done 2026-11-17; ShellAgenda.ViewGroupsItemsAndOffersNamedActions (hidden window, scripted host).
- [x] 5.2 Add reminders with lead times through the existing notification mechanism; verify timing with a fake clock and suppression in presentation mode.
  Status: done 2026-11-17; RemindsOnTheWorkingDayBeforeOnceAndHoldsDuringPresentations.
- [x] 5.3 Add Settings controls and en, cs, de strings; verify localized string checks.
  Status: done 2026-11-17; ui_tests (Strings.*) pass with the agenda.* and settings.agenda.* keys in en, cs and de.

## 6. Policy And Spec Coherence

- [x] 6.1 Add the `agenda` Policy bundle key and its four settings (design D7) to the policy parser and the control-plane policy schema; verify defaults and bounds with unit tests.
  Status: done 2026-11-17; client defaults in policy/core.cpp, control-plane/src/policy/keys.ts and the regenerated schema fixture; bounds are clamped in the service (min_confidence 0.5-0.95, daily_preparations 0-100).
- [x] 6.2 At archive, add "Agenda" and "Agenda item" to the glossary and `agenda` to the Policy bundle top-level keys in openspec/config.yaml; verify `openspec validate --all --strict`.
  Status: done 2026-11-19; glossary entries Agenda and Agenda item, the Agenda view in Inbox views, and `agenda` in the Policy bundle top-level keys.
