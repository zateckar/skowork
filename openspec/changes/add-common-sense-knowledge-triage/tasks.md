## 1. Gates And Rubric

- [x] 1.1 Add `common_sense_reject()` and `common_sense_reject_name()` (design D2). Verify with unit tests for each reason, the cs and de placeholders, and that useful facts from the observed data pass.
  Status: done 2026-11-21. Tests: AssistantItselfIsNeverAFact, PlaceholderParticipantsInThreeLanguages, MetaPredicatesOnlySaySomethingCameUp, SelfReferenceAndThreadFilePaths, UsefulObservedFactsPass.
- [x] 1.2 Apply the gates in `store_extraction()` and in the import preview before salience triage. Count rejects as dropped, and create no entity from a rejected name. Verify with storage tests that a user mark wins.
  Status: done 2026-11-21. Tests: GatesDropAgentWorkLogMetaAndPlaceholdersAndCreateNoEntitiesForThem, UserMarkWinsOverTheGates.
- [x] 1.3 Add the common-sense rubric to the extraction and rating instructions (design D1). Verify that both instructions contain it, and check it against the real model.
  Status: done 2026-11-21.
  - Test: ExtractionAndRatingInstructionsCarryTheRubric.
  - Real-model check recorded in design.md: three versions of the wording, and the final one rates 5 of 222 low, all of them noise.
  - The rating request shows each fact's statement. A fact the answer leaves out keeps its rating. The output limit is 2,048 tokens.

## 2. Cleanup

- [x] 2.1 Run the one-time cleanup driven by `triage.rules` (design D3). Verify with store tests that marked facts and user facts survive, and that the cleanup runs once.
  Status: done 2026-11-21. Tests: CleanupRemovesStoredNoiseOnceKeepsMarksAndRatesTheRestAgain, CleanupLeavesUnmarkedFactsForTheBackgroundRating.

## 3. Threads

- [x] 3.1 Strip the run-time line and use the Thread's title when indexing Threads (design D4).
  Status: done 2026-11-21. Test: RunContext.StripRunTimeRemovesOnlyTheLeadingTimeLine.

## 4. The User, Assistants, Duplicates, Listing

- [x] 4.1 Map "Me" and the user's names to the user's Person in extraction and at cleanup (design D5).
  Status: done 2026-11-21. Tests: SelfReferencesAreTheUser, MeAndTheUsersAccountNameAreTheSignedInUser, CleanupMergesAnExistingMeIntoTheUser.
- [x] 4.2 Reject AI assistant and Local App names in Threads (design D6).
  Status: done 2026-11-21. Tests: AssistantsAndLocalAppsOnlyInThreads, ThreadFactsAboutAssistantsAndLocalAppsAreNotKept.
- [x] 4.3 Add duplicate entity resolution with the client checks and the fact veto (design D7).
  Status: done 2026-11-21.
  - Tests: SimilarNamesFromTheTestMachine, EntityGroupsAreReadAndCheckedLikeOtherAnswers, DuplicateNamesAreMergedAsTheModelConfirmsAndAUserSplitStays, EntitiesAFactConnectsAreNeverMerged.
  - Real-model check recorded in design.md.
- [x] 4.4 Leave entities without facts out of the unfiltered Entities list (design D8).
  Status: done 2026-11-21. Test: EntitiesWithoutFactsAreNotListedButTheirPassagesAreFound.
  - A lookup by name still finds them, because Settings merge and split and the knowledge.entity tool rely on it.

## 5. Verification

- [x] 5.1 Run the knowledge, shell and runtime unit tests, and replay the observed facts.
  Status: done 2026-11-21 in the test-local build.
  - knowledge_analysis_tests: all passed.
  - win_tests with filter Knowledge*, Shell*Knowledge* and RunContext.*: all passed except ShellKnowledge.SaveMailAndChatAsNote. That test is flaky on the unchanged baseline too (it passed 1 of 3 runs with the change stashed).
  - Replay: 60 of 282 rejected (30 meta, 17 placeholder, 8 assistant, 4 local_app, 1 path), with "Me" mapped to the user.
- [x] 5.2 Check on the test machine after installing the build: the cleanup and merge log lines, re-rating, and Thread re-indexing within the budget.
  Status: done 2026-11-21 with release dev.30 to dev.34, recorded in design.md under Deployment On The Test Machine.
  - Fixed during the check: the rating batch size and output cap, queuing Threads for indexing, Local App spelling variants, rubric 2 for "Me".
  - New tests: CleanupIndexesThreadsAgain, the rubric-versus-rules case in CleanupLeavesUnmarkedFactsForTheBackgroundRating, and the "Passkey explainer app" spelling.
- [ ] 5.3 `openspec validate --all --strict`. At archive, rebase on the archived add-knowledge-salience requirements.
