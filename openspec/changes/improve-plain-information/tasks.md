## 1. Host

- [x] 1.1 Continuation blocker codes in the run store and `runs.recovery` (design D4); verify with a runtime test that a newer Run yields `newer_run`. Status: done 2026-11-27; ShutdownRecovery tests check `already_continued` and `stopped`.

## 2. Window

- [x] 2.1 Automation times from epoch values and schedules in words (D1, D2); verify with `describe_schedule` unit tests and the Automations shell tests. Status: done 2026-11-27; ShellFormat.CronSchedulesInWords, ShellAutomations.AgentCreatedTriggerIsLabelled ("Every Friday at 15:00", expression in the tooltip).
- [x] 2.2 Approval card Tool names and source references (D3); verify with a shell test. Status: done 2026-11-27; ShellActions.ApprovalCardShowsProvenanceAndEditThenApprove. The announcement and the recovery banner's unknown-outcome line use the same names.
- [x] 2.3 Recovery banner reasons from codes (D4); verify with a shell test. Status: done 2026-11-27; ShellRecovery.CannotContinueGivesTheReasonInWords.
- [x] 2.4 Skills Details toggle and translated risk; Policy expiry and translated decisions (D5); verify with the Settings shell tests. Status: done 2026-11-27; ShellPages.SkillsSectionShows… (Details toggle), ShellPages.PolicyPageAndFolderGrants; rendered and reviewed.
- [x] 2.5 Knowledge source switch and More menu, skipped items by file name (D5); verify with the Knowledge shell test. Status: done 2026-11-27; ShellKnowledge.SourcesFoldersExclusionsForgetMergeAndSplit; rendered and reviewed. The heading repeats the state only when paused or off by policy.
- [x] 2.6 Automation rows: the name opens the history, More menu (D5); verify with the Automations shell tests. Status: done 2026-11-27; ShellAutomations.EditorRunNowAndOpenResultFromHistory; rendered and reviewed.
- [x] 2.7 Knowledge view tabs with a selected state (D6); verify with the Knowledge view shell test. Status: done 2026-11-27; ShellKnowledge.WorkspaceTraverses… checks selected_state; the "(selected)" accessible name is kept.
- [x] 2.8 Team Chat header (Compact Chat Header); verify with the Chats shell test. Status: done 2026-11-27; ShellChats.EntriesConversationNewChatLeaveAndOff, ShellKnowledge.SaveMailAndChatAsNote; rendered and reviewed.
- [x] 2.9 One spacing for Settings lists (16 DIP multi-line, 8 DIP one-line). Status: done 2026-11-27.

## 3. Checks

- [x] 3.1 en, cs and de strings; verify with the ui_tests string check. Status: done 2026-11-27; ui_tests 110 pass (string check).
- [ ] 3.2 Run the client tests and `openspec validate --all --strict`. Status: partial 2026-11-27; ui_tests 110, win_tests 1109 (Pipe.* not run while the installed client holds the pipe), agenda, report, triage, teams pass. `openspec validate --all --strict` not run: no npm on this machine.
