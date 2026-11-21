# Tasks

## 1. Runtime

- [x] 1.1 At each limit, close the unexecuted calls with a limit result, make the final model call without tool definitions (design D1, D3), end the Run succeeded and record the `run_limit` Run event (design D2). Verify with unit tests for the tool_calls, model_calls and wall_minutes limits: no tool runs after the limit, the final request has no tools, and the Run ends succeeded with the answer. (RuntimeLoop.ModelCallLimitEndsWithAFinalAnswerWithoutTools, ToolCallLimitFillsInTheOpenResultsAndEndsWithAFinalAnswer, UnattendedRunsUseTheUnattendedLimits; WallTimeLimitUsesActiveTimeOnly covers the wall limit through its failing final call.)
- [x] 1.2 Fail the Run with the limit as reason when the final call fails or gives no answer (design D3). Verify with a unit test using an in-test fault for the final call. (RuntimeLoop.AFailedFinalCallFailsTheRunWithTheLimit.)
- [x] 1.3 Prefix a Child Run's answer at a limit for its parent (design D4). Verify with a unit test: the parent's tool result starts with the stopped-at-limit sentence. (RuntimeSubagents.AChildAtItsLimitReturnsAPartialAnswerMarkedForTheParent.)

## 2. Work inbox

- [x] 2.1 Show the Run Limit Note from the `run_limit` Run event, in en, cs and de, independent of Work Details Visibility. Verify with a feed unit test and a UI Automation check that the note is read by a screen reader. (ShellFeed.AnAnswerAtARunLimitIsFollowedByTheLimitNote, ShellFeed.RunLimitNoteShowsUnderTheAnswerWithWorkDetailsHidden.)

## 3. Verification

- [ ] 3.1 On the test machine, set policy.llm.run_limits.attended.tool_calls to 5 for a test Group, give a task that needs more calls, and record in design.md that the Thread shows an answer with the note and that replying continues the work.
- [ ] 3.2 Run `openspec validate --all --strict`.
