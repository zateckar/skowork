## 1. Streaming Markdown

- [x] 1.1 `close_streaming_markdown` and `MarkdownView::set_streaming_markdown`; the feed uses it for streaming answers (design D1). Verify with ui unit tests for open bold, italic, strikethrough, code span, partial link, escaped and spaced asterisks and open fences, and add the function to the Markdown fuzz target. Status: done 2026-11-27; StreamingMarkdown.* (4 tests, including every prefix of a sample answer) pass. A number in brackets ([2]) stays as written, since it is a Citation marker. The fuzz target checks it; the fuzz build (Linux) was not run here.

## 2. Durations

- [x] 2.1 The runtime stores `duration_ms` with streamed reasoning (design D2); verify with a runtime test using a streaming fake with timed reasoning deltas and one without. Status: done 2026-11-27; RuntimeLoop.StreamedReasoningStoresItsDuration passes.
- [x] 2.2 Tool card duration and JSON indentation, reasoning header "Thinking · N s" / "Thought for N s" (design D2); verify with feed and shell tests (header names) and the one-second timer test. Status: done 2026-11-27; ShellFeed.ToolCallsCarryTheirDurationAndRunningCallsTheirStart, ShellFeedLive.StreamingReasoningStartsAtItsFirstDelta and ShellFormat tests pass; ShellFeed.StreamsMarkdown… keeps "Thinking…" and "Reasoning" for times under one second. Rendered and reviewed offscreen.
- [x] 2.3 Step elapsed time on the status line from 2 s (design D2); verify with feed tests at 1 s and 12 s. Status: done 2026-11-27; the start time is covered by the feed tests above and the line was rendered offscreen. No dedicated window test at 12 s.

## 3. Command Palette

- [x] 3.1 `rank_palette` (design D3); verify with unit tests for prefix, word start, subsequence, accents, ties and the 50-item cap. Status: done 2026-11-27; ShellPalette.RanksPrefixThenWordStartThenSubsequence and .MatchingIgnoresCaseAndAccents pass.
- [x] 3.2 `CommandPalette` view, Ctrl+K, shortcut help entry (design D3); verify with a shell test that opens it, types, moves with Down, runs Enter, and with an accessibility check. Status: done 2026-11-27; ShellPalette.CtrlKFindsAThreadByTitleAndOpensIt passes (open, filter, nothing found, Enter opens a Thread, Escape, Settings, no accessibility problems).

## 4. Run Plan

- [x] 4.1 `update_plan` tool, availability, replay safety, prompt sentence (design D4); verify with runtime tests: valid plan, two running steps, too many steps, not offered to a Child Run, baseline catalog and schema budget tests updated. Status: done 2026-11-27; RuntimePlan.UpdatePlanAcceptsAValidPlanAndRefusesTwoRunningSteps, RuntimeLoop.MultiStepRunPersistsTheConversation and LazyToolLoading.* pass. RuntimeCompaction.LongRunIsCompacted… now uses a 12,200-token window: the extra core tool left one compaction pass short in the old 12,000 window.
- [ ] 4.2 Control-plane built-in catalogue entry and test pattern (design D4); verify with the tool catalogue test where Node is available. Status: partial 2026-11-27; `update_plan` added to BUILT_IN_TOOL_IDS and BUILT_IN_TOOL_DETAILS, the test recognises kUpdatePlan and this change's delta spec. Not run: no Node.js on this machine.
- [x] 4.3 Plan item in the feed fold and `PlanCard` (design D4); verify with feed tests (latest plan wins, placed at the first call, not done after stop) and a shell test of the list names with work details hidden. Status: done 2026-11-27; ShellFeed.RunPlanShowsTheLatestPlanWhereTheFirstWasMade and ShellFeed.RunPlanCardShowsTheLatestPlanWithWorkDetailsHidden pass.

## 5. Context Breakdown

- [x] 5.1 Context parts in the runtime and `threads.context`, breakdown and error level in the status bar (design D5); verify with a runtime test that the parts add up to `tokens` and a shell test of the indicator's name at 62 % and 92 %. Status: done 2026-11-27; RuntimeCompaction.FinalAnswerOverTheTriggerIsCompactedBeforeTheRunEnds checks that the parts add up, ShellStatusBar.ContextIndicatorListsWhatFillsTheContext passes.

## 6. Small Rules

- [x] 6.1 Composer: disabled Send/Queue/Steer on a blank draft, Stop primary (design D6); verify with shell tests. Status: done 2026-11-27; ShellComposer.EmptyDraftDisablesSendAndStopLeadsWhileRunning passes; ShellComposer.OfflineRequestWaitsForTheConnection checks Send after typing.
- [x] 6.2 Copy feedback on the button (design D6); verify with a shell test of the button's name after a click. Status: done 2026-11-27; ShellFeed.CopyShowsCopiedOnTheButtonThenReturns passes. Message, Chat message and code-block Copy use it.
- [x] 6.3 Avatar palette and stable colours (design D6); verify with a unit test for stability and the palette contrast test. Status: done 2026-11-27; UiTheme.AvatarColourIsStablePerName and UiTheme.PalettesMeetWcagAaContrast pass.
- [x] 6.4 Balanced wrapping in the text cache (design D6); verify with a ui test that a caption with an orphan keeps its line count and ends with two words. Status: done 2026-11-27; UiElements.WrappedTextDoesNotEndWithAnOrphanWord passes.

## 7. Checks

- [x] 7.1 en, cs and de strings; verify with the build-time string check. Status: done 2026-11-27.
- [ ] 7.2 Run the client tests, review the goldens, render the changed screens offscreen; `openspec validate --all --strict` where the CLI is available. Status: partial 2026-11-27; ui_tests 123 pass; win_tests 1198 of 1200 (Pipe.* not run), ShellDictation.DictateInserts… and Replay.BusyAndCancelKeepsWritten failed once and pass on three repeats. Goldens shell_thread_feed, shell_approval_card and shell_generated_image updated (avatar colour, disabled Send). Feed, plan card, palette and status bar rendered offscreen and reviewed. Not run: `openspec validate --all --strict` and the control-plane tests (no Node.js on this machine).
