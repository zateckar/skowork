# Spec Delta

## ADDED Requirements

### Requirement: File Append
files.write SHALL accept an optional `append` flag. With `append` set to true, it adds the content to the end of the file, and creates the file when it is missing. The write stays atomic and follows the rules of File Write Tools.

#### Scenario: File written in parts
- **WHEN** files.write writes "A" to a new file, then files.write writes "B" to it with append set to true
- **THEN** the file contains "AB"

#### Scenario: Append to a missing file
- **WHEN** files.write is called with append set to true on a path that does not exist
- **THEN** the file is created with the content
