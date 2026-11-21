# Tasks

## 1. Runtime

- [x] 1.1 Cap each tool result at the smaller of policy.llm.max_tool_result_kb and 25% of the context window of the next request's role, compared in calibrated estimated tokens (design D1, D3). Verify with unit tests: 40 KB offloaded at 32,768 tokens, 60 KB inserted at 262,144 tokens, and a 64 KB policy cap still applied when it is smaller. (RuntimeLargeResults.SmallWindowOffloadsAResultAboveAQuarter, LargeWindowKeepsTheResultBelowThePolicyCap, ChangedPolicyControlsAttachmentThresholdOnTheNextRun.)
- [x] 1.2 Offload the results of one model response largest first while their total exceeds 50% of the window (design D2). Verify with a unit test: four 20 KB results at 32,768 tokens give two offloaded and two inserted, appended in the order the model returned them. (RuntimeLargeResults.ParallelResultsStayWithinHalfTheWindow.)

## 2. Verification

- [ ] 2.1 On the test machine against gamma, ask for a summary of four large local text files read in parallel, and record in design.md that the Run no longer fails with CONTEXT_LENGTH.
- [ ] 2.2 Run `openspec validate --all --strict`.
