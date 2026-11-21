# Tasks

## 1. Tools

- [x] 1.1 Add `replace_all` to the files.edit definition and replace every non-overlapping occurrence in one atomic write, keeping the file's encoding (design D1, D2). Verify with unit tests: five occurrences replaced with a count of five, no occurrence fails with the file unchanged, a UTF-16 file keeps its encoding, and a call without the flag still fails on two occurrences. (FileTools.ReplaceAll*, FileTools.EditReplacesExactlyOneSpan.)

## 2. Verification

- [ ] 2.1 On the test machine, ask the agent to rename a function used several times in a Local app file, and record in design.md that it uses one files.edit call with replace_all.
- [ ] 2.2 Run `openspec validate --all --strict`.
