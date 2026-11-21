# Design

## Context

- files.write creates or replaces a file atomically, under a per-path lock shared with files.read and files.edit. files.edit already fails unless its text span is found, so it cannot destroy content it did not see.
- files.read returns at most policy.tools.files.max_read_kb (default 256 KB) and says when it truncated. Documents (docx, xlsx, pptx, pdf) are read through Document Text Extraction, so the model sees their text, not their bytes.
- A Thread can have several Runs, and Child Runs run in the same Thread.

## Decisions

### D1: The record is per Thread and survives restarts

The client keeps, per Thread and canonical path, the state of the file as last seen: after a full files.read, after files.write, after apps.create writes its scaffold (app.json, the entry file and public\index.html), and after a files.edit of a file that was seen before the edit. An edit of an unseen file records nothing: it showed the agent one span, not the file. A later Run in the same Thread, or a Child Run, can overwrite a file read earlier in that Thread. The record is stored with the Thread, so a restart or an approval that waits overnight does not lose it. Deleting the Thread deletes the record.

Alternatives considered:
- Per Run: a user who says "now replace it" in the next message would force a second read for nothing.
- Per client, across Threads: another Thread's read does not mean this conversation's model saw the content.

### D2: A file's state is its size, last-write time and SHA-256

The check at files.write compares the current size and last-write time with the record, and when both match, the SHA-256 of the current bytes. Hashing an 8 MB file takes well under 50 ms on the baseline i5, and only happens on overwrite.

Alternatives considered:
- Last-write time only: coarse on some network shares, and tools can restore it.
- Hash only: reads the whole file even when the size already shows a change.

### D3: Only a full text read counts

A files.read with a line range, a read that was truncated at the read limit, and a read through Document Text Extraction do not count as seen. For documents this means files.write never overwrites them: it writes UTF-8 text, and writing text over a Word or Excel file destroys it either way. A text file larger than the read limit cannot be overwritten either; the error tells the model to use files.edit or write a new file.

Alternatives considered: let a set of range reads covering the whole file count. That is complex to track and rare in practice.

### D4: The error tells the model what to do

The call fails before anything is written, with one of:
- "<path> exists and has not been read in this Thread. Read it in full with files.read before replacing it, or write to a new file."
- "<path> changed after it was last read in this Thread. Read it again before replacing it."
- "<path> is too large or not a text file, so it cannot be replaced with files.write. Use files.edit or write to a new file."

## Risks

- [An extra files.read before each overwrite costs a model call] -> Only for files the agent did not create or read in the Thread. Local apps, which the agent writes itself, are not affected.
- [The user wants a file replaced without the agent reading it] -> The agent reads it first; the read is cheap compared with losing the file.
- [The record grows in long Threads] -> One row per path per Thread; it is removed with the Thread.
