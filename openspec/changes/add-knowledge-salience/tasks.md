## 1. Validation Spikes

- [ ] 1.1 Build a labelled corpus of at least 120 passages in en, cs and de (Graph-mock mail, calendar, meeting transcripts, Chats, files; no real mailbox) with facts labelled important, temporary or trivial, including a company-wide memo, project mail and lunch or thank-you chatter. Run the extended extraction instruction against the real gateway; verify recall of important facts is at least 0.9, at least 70% of trivial facts are dropped, and record the relation-count reduction in design.md.
  Status: not started 2026-11-17; needs the real gateway, and unsigned builds on this workstation cannot open outbound connections (use dev-proxy with SKOWORK_DEV_PROXY, unsandboxed).
- [ ] 1.2 Tune the retrieval weights (design D5) on 30 labelled queries over the corpus; verify the memo outranks small talk for related queries without lowering recall@10 versus the current ranking.
  Status: not started 2026-11-17; depends on the corpus in 1.1 and the real gateway. Weights are in place as design constants (see RetrievalWeightsRankImportantSourcesHigher).
- [ ] 1.3 Measure the added output tokens and latency per extraction call against the real gateway; verify the daily budget still covers a typical day of new items.
  Status: not started 2026-11-17; needs the real gateway (same network constraint as 1.1).

## 2. Extraction

- [x] 2.1 Add client-derived source metadata to each passage header (design D1); verify with unit tests that metadata never comes from passage text and that a passage cannot spoof a header.
  Status: done 2026-11-17; PassageHeadersCarryClientMetadataAndNotPassageText, MailHeaderNamesTheSenderAndBreadthFromItsMetadata, PassageHeaderIsWrittenByTheClient, PassageTextCannotForgeAHeaderOrCloseAPassage, PassageAttributesStayOnOneLine, AddressingBreadthFollowsRecipientCount, SourceContextRoundTripsThroughItsJson.
- [x] 2.2 Extend the extraction schema and parser with salience, durability and statement (design D2, D3); verify with parser unit tests and the existing extraction fuzz target extended to the new fields.
  Status: done 2026-11-17; SchemaAsksForSalienceDurabilityAndStatement, ParserReadsRatingsAndDefaultsMissingOnes, StatementIsCutAtAWordBoundaryWithinTheLimit, StatementCollapsesWhitespace, RelationScoresAreReadAndOutOfRangeEntriesIgnored, DurabilityWordsAreNormalised. Fuzz target client/fuzz/kg_chunk_fuzz.cpp extended with the invariant checks; the fuzzer itself was not run.
- [x] 2.3 Apply the threshold and drop trivial relations; count dropped candidates per Source; verify with storage tests including policy.knowledge.min_salience bounds and that orphan entities are not created.
  Status: done 2026-11-17; MinSalienceIsReadWithItsBounds, TriageKeepsImportantFactsCountsWhatWasNotKeptAndCreatesNoOrphans, ThresholdFromPolicyDecidesWhatIsKept, NewFactsAreRatedByTheExtractionThatStoredThem.
- [x] 2.4 Give temporary relations a default valid-to of 30 days (design D4); verify that they leave current answers and remain available as history.
  Status: done 2026-11-17; TemporaryFactsLeaveCurrentAnswersAfterThirtyDays.

## 3. Retrieval And View

- [x] 3.1 Apply salience and source weights after reranking (design D5); verify ranking unit tests and that low-importance facts remain retrievable by explicit query.
  Status: done 2026-11-17; RetrievalWeightsRankImportantSourcesHigher, StatementsShowWithTheirPassageAndLowImportanceStaysRetrievable.
- [x] 3.2 Show statements and importance levels in the Knowledge view and knowledge.search results; add mark important and mark unimportant and a Low importance filter (design D6); verify shell tests, UI Automation names and that marks survive re-extraction.
  Status: done 2026-11-17. Statements and importance levels in the view and search; mark important and unimportant (knowledge.fact.importance); a Low importance list (knowledge.facts with importance low) where a fact can be restored or forgotten; named controls. UserMarksSurviveReExtractionAndNeverDeleteTheFact, ImportanceIsShownAsALevelNotANumber, ShellKnowledge.LowImportanceListRestoresAndForgetsFacts.
- [x] 3.3 Add en, cs and de strings; verify localized string checks.
  Status: done 2026-11-17; knowledge.importance.high/normal/low and knowledge.mark_important/mark_unimportant added to en, cs and de. Strings.BuiltInBundlesLoadAndAgree and Strings.ConnectorKeysAreRenamedInEveryLanguage pass (ui_tests, 6/6 Strings tests).

## 4. Backfill

- [x] 4.1 Score existing facts in the background within 20% of the extraction budget, newest first, never deleting (design D7); verify with a fake clock and budget tests and that unscored facts read as 0.5.
  Status: done 2026-11-17; ExistingFactsAreRatedInTheBackgroundWithinTheScoringShare.

## 5. Spec Coherence

- [ ] 5.1 At archive, rebase on the archived Entity And Relation Extraction, Hybrid Retrieval With Provenance and Retrieval Respects Knowledge State requirements; verify `openspec validate --all --strict`.
