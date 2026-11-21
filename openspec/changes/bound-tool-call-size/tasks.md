# Tasks

## 1. Runtime

- [x] 1.1 Append the tool call size instruction (design D1) to the system prompt of Runs and subagent Runs. Verify with a unit test that a top-level and a subagent request both contain it.

## 2. Tools

- [x] 2.1 Add `append` to files.write (design D2), atomic and under the path lock. Verify with unit tests: append to an existing file, append creates a missing file, and a write without append still replaces the file.

## 3. Verification

- [ ] 3.1 Re-run the passkeys Local app request against gamma and record in design.md whether the model splits its writes and the Run finishes without TIMEOUT.
- [ ] 3.2 Run `openspec validate --all --strict`.
