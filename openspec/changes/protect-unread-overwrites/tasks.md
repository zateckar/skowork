# Tasks

## 1. Storage

- [x] 1.1 Add a per-Thread record of seen file states (canonical path, size, last-write time, SHA-256), deleted with its Thread (design D1, D2). Verify with unit tests: a record survives reopening the store, and deleting the Thread removes its records. (Migration 2200 `thread_file_states`; InboxDelete.ForgetCascadeRemovesTheThreadItsAttachmentsAndIndexEntries.)

## 2. Tools

- [x] 2.1 Record the file state after a full, untruncated files.read of a text file, after files.write, after apps.create writes its scaffold, and after files.edit of a file that was seen; record nothing for ranged, truncated or extracted reads (design D1, D3). Verify with unit tests for each kind of read. (FileTools.RangedAndTruncatedReadsDoNotCountAsSeen, AnEditOfAnUnseenFileDoesNotMakeItSeen, ACreatedFileRecordedForTheThreadCanBeReplaced; AppsTools tests with the shared record.)
- [x] 2.2 Check the record in files.write before replacing an existing file, and fail with the errors of design D4. Leave creation of a missing file and File Append unchecked. Verify with unit tests for every scenario of Overwrite Protection. (FileTools.OverwriteWithoutAReadFailsAndKeepsTheFile, OverwriteAfterAFullReadInTheSameThread, OverwriteFailsWhenTheFileChangedAfterTheRead, DocumentsAreNeverReplacedWithFilesWrite.) A Child Run shares its Thread id, so it shares the record by construction; no separate test.

## 3. Verification

- [ ] 3.1 On the test machine, let the agent read a text file, change the file in Notepad, then ask the agent to replace it, and record in design.md that the call fails, the change is kept, and the agent reads the file again.
- [ ] 3.2 Run `openspec validate --all --strict`.
