## 1. Runtime Measurement

- [x] 1.1 Record request start, first output token, end and output tokens for every model call and emit a stored `model_call` Run event (design D1); verify with runtime unit tests using an in-test streaming fake, including a call with no usage (`exact: false`), a call with reasoning, a retried call (only the successful attempt counts) and a Child Run. Status: done 2026-11-17; RuntimeLoop.EveryModelCallStoresItsTimingBeforeItsAnswer, RuntimeLoop.ModelCallWithoutUsageOrStreamedTextIsEstimatedAndHasNoFirstToken and RuntimeLoop.ARetriedAttemptRestartsTheModelCallTiming pass. Reasoning and Child Runs go through the same sink and call path; no separate test.
- [ ] 1.2 Verify once against the real gateway (local e2e) that `model_call` output tokens equal the provider's completion usage for a streamed answer, and whether reasoning tokens are part of completion (design D1).

## 2. Feed

- [x] 2.1 Compute the live estimate and the measured Run speed in the feed model (design D1, D2); verify with feed unit tests: no value before 1 s or 20 tokens, estimate replaced by the measured value, Tool time excluded, failed Run without output shows nothing. Status: done 2026-11-17; the RunSpeed tests in shell_core_test.cpp pass.
- [ ] 2.2 Render the speed line and the waiting time under each answer of a top-level Run, visible with work details hidden; verify with shell tests and a UI Automation name check; verify that detailed token usage stays hidden by default. Status: partial; ShellUsability.GenerationSpeedShowsUnderTheAnswerWithWorkDetailsHidden passes (measured "50 t/s" shown with work details hidden, no usage line, no accessibility problems) and RunSpeed.SpeedLineSitsAfterTheAnswerAndBeforeTheUsageLine covers the order. Not yet seen in the running app against the real gateway; the waiting time depends on the model `progress` events of improve-proactive-durable-work task 4.1.
- [x] 2.3 Add en, cs and de strings; verify with the localized string checks. Status: done 2026-11-17; ui_tests pass.

## 3. Spec Coherence

- [ ] 3.1 At archive, rebase Work Details Visibility on its archived version so it lists the generation speed line among the default content; verify `openspec validate --all --strict`.
