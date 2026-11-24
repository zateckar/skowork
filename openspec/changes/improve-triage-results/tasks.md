# Tasks

## 1. Classification and storage
- [x] 1.1 Extend the classifier response with action and category (design D1), invalid when missing for a relevant item. Verify with classifier tests. Status 2026-11-24: done; `TriageClassifier.*` (triage_tests).
- [x] 1.2 Migration 2600: triage_items action, category, reported_at. Verify with the storage migration tests. Status 2026-11-24: done; also strips "Suggestion: " from existing triage Thread titles; migrations run in every win_tests storage test.

## 2. Results
- [x] 2.1 Titles without the "Suggestion: " prefix (D5). Verify with a triage test. Status 2026-11-24: done; `EventTriage.RelevantItemBecomesAnUnreadSuggestionThreadThatNeverStartsItself`.
- [x] 2.2 Action items: Thread with action and category in its origin ref, then automatic preparation as an Unattended read-only Run with the cap and the policy check, and a setting to turn it off (D2). Verify with triage tests: prepared, cap reached, turned off, new revision does not prepare twice. Status 2026-11-24: done; `EventTriage.AnActionItemIsPreparedAutomaticallyAsAReadOnlyUnattendedRun`, `EventTriage.PreparationWaitsForStartWorkWhenOffForbiddenOrCapped`.
- [x] 2.3 Information items: no Thread; digest(now) with unreported information items and open action items; marked reported by the Daily report (D3). Verify with a triage test. Status 2026-11-24: done; `EventTriage.InformationItemsGetNoThreadAndAreReportedOnce`.

## 3. Shell and report
- [x] 3.1 Status and category badges instead of the system origin badge in the list and Thread header (D4). Verify with a shell test. Status 2026-11-24: done; `ShellModel.TriageThreadsShowStatusAndCategoryInsteadOfTheOriginBadge` (ui_tests); list and header drawn from the same helper.
- [x] 3.2 The Daily report digest lists triage information items and open action items. Verify with a report test. Status 2026-11-24: done; `DailyReportCore.TriageResultsAreTheirOwnSections`.
- [x] 3.3 Settings > Triage: "Prepare action items automatically" switch; strings en, cs, de. Status 2026-11-24: done; `ShellTriage.SettingsSectionAndSuggestionCard` uses the switch.

## 4. Acceptance feedback (2026-11-24)
- [x] 4.2 One status badge: category only from triage_badges, "Needs your action" also for a triage action Thread without a Run (D8). Verify with a shell model test. Status 2026-11-24: done; `ShellModel.TriageThreadsShowStatusAndCategoryInsteadOfTheOriginBadge`.
- [x] 4.3 Collapsed source card for untrusted-start messages (D9). Verify with a feed model test. Status 2026-11-24: done; `ShellFeed.ARunFromASourceShowsOnlyTheSourceCollapsed`.
- [x] 4.4 Preparation preamble: summary, ask_user options, then reply checked and sent through mail.send with reply_to (approval), Teams text only (D6). Verify with a triage test on the preamble. Status 2026-11-24: done; `EventTriage.AnActionItemIsPreparedAutomaticallyAsAReadOnlyUnattendedRun`, `EventTriage.ThePreparationOfATeamsItemNeverSendsMail`.
- [x] 4.5 No action needed: triage.no_action records it, cancels the Run, archives the Thread, and a new revision keeps it (D7). Verify with triage tests and the shell suggestion card test. Status 2026-11-24: done; migration 2700; `EventTriage.NoActionNeededIsRecordedCancelsTheRunAndArchives`.

- [x] 4.6 Invalid response: halve the next batch down to 1, set a single failing event aside as uncertain, double the size again after success, name the failed check, accept null action and category (D10). Verify with triage and classifier tests. Status 2026-11-24: done; `EventTriage.AnInvalidResponseShrinksTheBatchUntilTheBadEventIsSetAside`, `EventTriage.AnUnavailableGatewayKeepsTheBatchAndBacksOff`, `TriageClassifier.AcceptsNullActionAndCategoryAndNamesTheFailedCheck`.
- [x] 4.7 Crash fix (access violations on the UI thread, 2026-11-24, traced with an unstripped copy of the installed build): ThreadPane::rebuild cleared its children but left `suggestion_` and `progress_` pointing at them on the early returns (new Thread, none open, load error), so the next triage.changed rebuilt a freed suggestion card. Both are now reset with `banner_`. Also the feed's citation knowledge.open callback checks that the view is still alive. Verify with `ShellTriage.SettingsSectionAndSuggestionCard` (leaves the Thread and fires triage.changed) and the ui suite.

## 5. Acceptance
- [ ] 5.1 On the user's laptop: new relevant mail produces either a prepared Thread with badges and a draft, or an entry in the next Daily report; nothing is written to Outlook. Installed 2026-11-24 17:04; waiting for new mail on the user's laptop. `openspec validate` not run (no Node).
