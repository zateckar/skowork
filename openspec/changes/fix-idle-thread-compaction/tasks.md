## 1. Test

- [x] 1.1 Write a unit test in `client/tests/runtime/runtime_test.cpp`: four Runs in one Thread, where the fourth Run's request is below the trigger and its final answer brings the Thread above it. Verify: the test fails before the change (no compaction record, context usage at or above the trigger) and passes after it (an automatic record that names the fourth Run, usage below the trigger, the Run succeeded).
- [x] 1.2 Write a unit test: the same Thread with a summarizer that fails. Verify: the fourth Run ends as succeeded with its answer, and the Activity log records "Compaction failed".

## 2. Implementation

- [x] 2.1 Implement D1 to D3 in `client/src/runtime/runtime.cpp`: check the trigger after the final answer of a top-level Run, before it ends as succeeded. Verify: tests 1.1 and 1.2 pass, and `win_tests --gtest_filter=Runtime*:StablePromptPrefix*` passes.

## 3. Build, test and validate

- [x] 3.1 Build the debug tests and the release client. Verify: the build succeeds, the runtime tests pass, and the results are recorded in design.md Acceptance.
- [ ] 3.2 Run `openspec validate --all --strict`. Verify: it passes with no warnings. Status: not run on 2026-11-24 because Node is not installed on this machine.
- [ ] 3.3 After every task above is checked, run `openspec archive fix-idle-thread-compaction --yes`, then reread the merged specs under `openspec/specs/client/` for coherence.
