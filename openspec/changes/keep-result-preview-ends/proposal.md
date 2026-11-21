# Proposal

## Why

When a tool result is larger than the cap of Large Tool Results, the model gets a preview of the first 4 KB and an attachment reference. For many results the important part is at the end: the error and exit summary of shell.run, the last log lines, the totals of a report, the final status of a build. The model sees the beginning, which is often a banner or progress output, and has to spend another call reading the attachment to find out what happened. When the attachment cannot be stored, the result is cut at the cap and the end is lost entirely.

Other agent harnesses keep both ends: one keeps the first and last part of long command output around an omission marker, another does the same for failed commands.

## What Changes

- A preview holds the start and the end of the result, about half each, with a marker between them that states how many bytes were left out and how to read them.
- Cut points fall on line breaks where one is near, and never inside a UTF-8 character.
- The same shape applies when the attachment cannot be stored and the result is cut at the cap.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `client/agent-runtime`: Tool Result Previews (added), Tool Result Cut Without Attachment (added).

## Impact

- Roadmap: Hardening, after improve-transcript-language. Depends on add-agent-runtime (Large Tool Results). Independent of `cap-tool-results-by-context`, which changes when a preview is used, not its shape.
- Code: the preview built in the runtime's tool-result path (`client/src/runtime/runtime.cpp`). The preview size stays 4 KB. No control-plane or UI change.

Out of scope: the files.read truncation of client/core-tools, which returns the first part with a notice on how to read further ranges, and stays that way because reading continues from where it stopped.
