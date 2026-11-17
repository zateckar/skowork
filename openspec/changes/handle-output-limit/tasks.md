# Tasks

## 1. Model access

- [x] 1.1 Mark repaired tool calls of a `length` response invalid with the cut-off error (design D3); unit tests for a cut-off call and for a complete call before it.

## 2. Runtime

- [x] 2.1 Continue after an output-limit response without a valid tool call, with the request-only note (design D1); unit test that the note is sent once and not stored.
- [x] 2.2 Fail the Run with `output_limit` after two consecutive continuations, and reset the count on a normal response (design D2); unit tests.

## 3. Verification

- [ ] 3.1 Re-run the real-gateway Local app evaluation (AppE2E.ModelBuildsLocalAppsAndUsesGeneratedSurfaces) on a role with a 4096-token output limit and record the outcome in design.md.
