# Proposal

## Why

files.write replaces an existing file's whole content. Nothing checks that the agent has seen that content. The model can overwrite a file it never read, for example because it assumed a name was free, or a file it read an hour ago that the user has since changed. Skowork's users keep working on their documents while the agent runs, so both cases happen. Policy decides whether the write may happen, but not whether the agent knows what it replaces, and files.write is often allowed without approval. The result is silent data loss in the user's own files, the worst outcome a write tool can have.

Another agent harness refuses to overwrite an existing file that the agent has not fully read in the session, and refuses when the file changed after the read.

## What Changes

- files.write replaces an existing file only when, in the same Thread, the client has seen the file's current content: its last full files.read returned it, or the agent's own last files.write or files.edit wrote it, and the file has not changed since.
- A read with a line range, a truncated read, or a read through Document Text Extraction does not count.
- Otherwise the call fails with an error saying why, and the file is unchanged. The model can read the file and try again.
- Creating a file that does not exist, and appending to a file (File Append), are not affected.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `client/core-tools`: Overwrite Protection (added), Seen File Content (added).

## Impact

- Roadmap: Hardening, after improve-transcript-language. Depends on add-core-tools-and-mcp (File Read Tools, File Write Tools, Document Text Extraction). Refers to File Append, added by `bound-tool-call-size`.
- Code: the files.read, files.write and files.edit paths (`client/src/tools/files.cpp`) and a per-Thread record of seen file states in the client store. No control-plane or UI change.

Out of scope: files.edit, which already applies only to content it finds exactly; files written by shell.run or by other tools; files the user changes while an approval for files.write is pending, which the check at execution time covers.
