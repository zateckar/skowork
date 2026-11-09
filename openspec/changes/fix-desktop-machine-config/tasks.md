# Tasks

## 1. Reproduction
- [x] 1.1 Add isolated loader regressions and reproduce rejection of BOM-marked UTF-16 before changing the loader; verify the README UTF-8 example and executable-relative lookup as controls.

## 2. Implementation
- [x] 2.1 Support the specified encodings and source-specific diagnostics, with regression tests for malformed input, missing/unreadable files, extra `.txt`, defaults, registry precedence and production URL validation.

## 3. Validation
- [ ] 3.1 Build the release desktop client, run configuration regressions and relevant existing tests, and record results in design.md; run OpenSpec strict validation.

Once all tasks are complete, archive this change and reread the merged spec as AGENTS.md requires.
