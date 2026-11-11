## 1. Child Run waits (client/agent-runtime)

- [x] 1.1 Write unit tests in `client/tests/runtime/runtime_test.cpp` with `FakeClock`: an attended child Run's Approval is rejected after 30 minutes, and the child then continues with the expiry text. Verify: the tests fail before the change and pass after it with `win_tests --gtest_filter=*ChildRun*`.
- [x] 1.2 Write a unit test: an unanswered `ask_user` in a subagent ends the child as failed with reason "subagent time limit reached" at 60 minutes, its open Action item is cancelled, and the parent gets the failure as the call's result. Verify: the test passes.
- [x] 1.3 Write a unit test: an Approval of an Unattended run's child Run is still open after 2 hours. Verify: the test passes and no timer resolves it.
- [x] 1.4 Implement the expiry (D2) and the time limit (D3) in `client/src/runtime/runtime.cpp`, on the runtime clock. Verify: tests 1.1 to 1.3 pass, and the existing `runtime_test.cpp` cases still pass.

## 2. Child Run cards in the feed (client/work-inbox)

- [x] 2.1 Write a unit test in `client/tests/shell/shell_core_test.cpp`: `build_feed` with an open Approval of a child Run returns an action card under that child's subagent entry. Verify: the test fails before the change and passes after it.
- [x] 2.2 Write a unit test in `client/tests/inbox/inbox_test.cpp`: `threads.feed` returns the events of an interrupted child Run, and does not return the events of a child Run without open Action items. Verify: the test passes.
- [x] 2.3 Implement D1 in `client/src/inbox/inbox_service.cpp` (load the events of interrupted child Runs) and `client/src/shell/feed.cpp` (place their Action items under the subagent entry). Verify: tests 2.1 and 2.2 pass.
- [ ] 2.4 Acceptance, needs the user's OK because it drives the desktop client and the user's own Skill "internal-policy-lookup": start the Skill from a Thread, confirm the Approval card appears under the Skill's entry, approve it, and confirm the Run resumes. Verify: the Run finishes after the approval, and the result is recorded in design.md Acceptance.

## 3. Local app start (client/local-apps)

- [x] 3.1 Write unit tests in `client/tests/apps/apps_test.cpp` with a fixture app that never reports ready and a start timeout of 500 ms: `folders_for`, `create` and `update` each return within 200 ms while the start is pending. Verify: the tests fail before the change (they wait for the timeout) and pass after it.
- [x] 3.2 Write a unit test: two concurrent `connect` calls for one stopped app start one process, and both get the same start result. Verify: the test passes.
- [x] 3.3 Write a unit test using the tools fixture in `apps_test.cpp`: an approved `files.write` into an app folder completes within 2 seconds while another app's start is pending. Verify: the test passes.
- [x] 3.4 Implement D4 in `client/src/apps/apps.cpp` and `apps.h`: wait for the ready signal without holding `AppManager::mx_`, with a per-app starting state. Verify: tests 3.1 to 3.3 pass, and the existing `AppsManager` and `AppsServer` tests still pass.
- [ ] 3.5 Acceptance, needs the user's OK because it creates a Local app on the desktop: have the agent build an app, approve a write to its folder, and time the write. Verify: the write completes within 2 seconds, and the result is recorded in design.md Acceptance.

## 4. Build, test and validate

- [x] 4.1 Build and run the client tests: `cmake --build --preset test` in `client/`, then run `client/build/test/bin/win_tests.exe` and `client/build/test/bin/ui_tests.exe`. Verify: all tests pass, and the results are recorded in design.md Acceptance.
- [x] 4.2 Run `openspec validate fix-tool-waits --strict` and `openspec validate --all --strict`. Verify: both pass with no warnings.
- [ ] 4.3 After every task above is checked, run `openspec archive fix-tool-waits --yes`, then reread the merged specs under `openspec/specs/client/` for coherence. Verify: the archive succeeds and the merged specs read consistently.
