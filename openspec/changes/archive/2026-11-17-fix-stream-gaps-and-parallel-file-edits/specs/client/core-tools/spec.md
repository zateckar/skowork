# Spec Delta

## MODIFIED Requirements

### Requirement: File Write Tools
The client SHALL provide files.write, which creates or overwrites a file, and files.edit, which replaces an exact text span. Both are write-local tools. They apply only to paths permitted by policy.filesystem.write. files.edit MUST fail when the text span is not found exactly once. Calls of files.read, files.write and files.edit on the same file SHALL run one at a time, so that each files.edit applies to the content left by the call before it.

#### Scenario: Ambiguous edit
- **WHEN** files.edit is called with a text span that occurs twice in the file
- **THEN** the call fails with an "ambiguous match" error and the file is unchanged

#### Scenario: Write outside permitted folders
- **WHEN** files.write targets a path not covered by policy.filesystem.write
- **THEN** the call is denied and the file is not created

#### Scenario: Parallel edits on one file
- **WHEN** one model turn contains two files.edit calls on different spans of the same file and they run in parallel
- **THEN** both calls succeed and the file contains both changes
