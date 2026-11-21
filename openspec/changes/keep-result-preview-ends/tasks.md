# Tasks

## 1. Runtime

- [x] 1.1 Build previews from the first and last half of the preview size with the marker between them, cutting at a line break within 256 bytes or else at a UTF-8 boundary (design D1, D2). Verify with unit tests: multi-line output keeps its last lines, a single 500 KB line is cut at character boundaries, and multi-byte characters at both cuts stay valid UTF-8. (RuntimePreviewCut.*, RuntimeLargeResults.CommandOutputKeepsTheErrorAtTheEnd, TwoMegabyteResultBecomesAPreviewAndAnAttachmentReadInParts.)
- [x] 1.2 Apply the same split when the attachment cannot be stored (design D1). Verify with a unit test whose attachment store fails: the inserted text holds start, marker and end within the cap. (RuntimeLargeResults.ResultCutWithoutAttachmentKeepsStartAndEnd.)

## 2. Verification

- [ ] 2.1 On the test machine, run a shell.run command that prints more than the cap and then fails, and record in design.md that the model's next step refers to the error without reading the attachment.
- [ ] 2.2 Run `openspec validate --all --strict`.
