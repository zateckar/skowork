# Spec Delta

## ADDED Requirements

### Requirement: Overwrite Protection
files.write SHALL replace an existing file only when the client has seen the file's current content in the same Thread (Seen File Content). Otherwise the call MUST fail without changing the file, with an error that states the reason and how to proceed. Creating a file that does not exist, and appending with File Append, are not affected.

#### Scenario: Overwrite without a read
- **WHEN** files.write targets an existing file that was not read in the Thread
- **THEN** the call fails with an error saying that the file must be read in full first, and the file is unchanged

#### Scenario: Overwrite after a full read
- **WHEN** the agent reads a text file in full and then calls files.write on it, and the file has not changed in between
- **THEN** the file is replaced

#### Scenario: Word document
- **WHEN** the agent reads a docx file through Document Text Extraction and then calls files.write on it
- **THEN** the call fails with an error saying that the file cannot be replaced with files.write, and the document is unchanged

### Requirement: Seen File Content
The client SHALL treat a file's content as seen in a Thread when, in that Thread, the last files.read of it returned it in full, files.write or apps.create wrote it, or files.edit changed it while it was seen, and its size, last-write time and content have not changed since. A files.read with a line range, a truncated files.read and a read through Document Text Extraction do not count. Child Runs share their Thread's record, and the record survives client restarts.

#### Scenario: File changed after the read
- **WHEN** the agent reads a text file in full, the user then saves a change to it, and files.write targets it
- **THEN** the call fails with an error saying that the file changed after it was read, and the user's change is kept

#### Scenario: The agent's own file
- **WHEN** files.write creates a file and a later files.write in the same Thread replaces it, with no change by anyone else in between
- **THEN** the file is replaced without a read

#### Scenario: Partial read
- **WHEN** the agent reads lines 1 to 50 of an existing file and then calls files.write on it
- **THEN** the call fails and the file is unchanged

#### Scenario: Edit of an unseen file
- **WHEN** files.edit changes one span of an existing file that was not read in the Thread, and files.write then targets it
- **THEN** the files.write call fails, because the edit did not show the agent the rest of the file

#### Scenario: Local app scaffold
- **WHEN** apps.create creates a new app with its starter page, and files.write in the same Thread then replaces the starter page
- **THEN** the page is replaced without a read

#### Scenario: Read by a Child Run
- **WHEN** a Child Run reads a text file in full and its parent Run then calls files.write on it, with no change in between
- **THEN** the file is replaced
