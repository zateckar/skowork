# Spec Delta

## MODIFIED Requirements

### Requirement: File Write Tools
The client SHALL provide files.write, which creates or overwrites a file, and files.edit, which replaces an exact text span. Both are write-local tools and apply only to paths permitted by policy.filesystem.write. files.edit MUST fail when the text span is not found exactly once, unless replace_all is set (Edit All Occurrences). Calls of files.read, files.write and files.edit on one file SHALL run one at a time, so each files.edit applies to the content left by the call before it.

#### Scenario: Ambiguous edit
- **WHEN** files.edit is called with a text span that occurs twice in the file
- **THEN** the call fails with an "ambiguous match" error and the file is unchanged

#### Scenario: Write outside permitted folders
- **WHEN** files.write targets a path not covered by policy.filesystem.write
- **THEN** the call is denied and the file is not created

#### Scenario: Parallel edits on one file
- **WHEN** one model turn contains two files.edit calls on different spans of the same file and they run in parallel
- **THEN** both calls succeed and the file contains both changes

## ADDED Requirements

### Requirement: Edit All Occurrences
files.edit SHALL accept an optional `replace_all` flag. When it is true, files.edit replaces every non-overlapping occurrence of the text span in one write, fails without changing the file when there is none, and its result states how many occurrences it replaced.

#### Scenario: Replace every occurrence
- **WHEN** files.edit is called with replace_all set to true and a text span that occurs five times in the file
- **THEN** all five occurrences are replaced in one write, and the result states that five occurrences were replaced

#### Scenario: Replace every occurrence without a match
- **WHEN** files.edit is called with replace_all set to true and a text span that does not occur in the file
- **THEN** the call fails with a "not found" error and the file is unchanged
