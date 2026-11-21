# Design

## Decisions

### D1: One boolean flag, no occurrence index

files.edit takes `path`, `old_text`, `new_text` and now an optional boolean `replace_all` (default false). The definition says: "Set replace_all to replace every occurrence, for example to rename something; the result states how many were replaced."

Alternatives considered:
- An `occurrence` index (replace the Nth match): models miscount occurrences, and adding surrounding text already makes a single match unique.
- Regular-expression replacement: more power than a rename needs, and a wrong pattern changes far more than intended.
- Leave renames to shell.run: depends on the shell mode, and a PowerShell replace does not keep the file's encoding the way files.edit does.

### D2: Non-overlapping, left to right, one atomic write

Occurrences are found left to right in the current content and do not overlap; replaced text is not searched again. All replacements are written in one atomic write under the same path lock as other files.edit calls, and the file keeps its encoding.

## Risks

- [The span also occurs inside other words, so too much is replaced] -> The definition asks for distinctive text, and the result states the number of replacements, so the model can see an unexpected count and correct it.
- [The tool definition changes the prompt prefix] -> It changes once with the release, the same as any tool change.
