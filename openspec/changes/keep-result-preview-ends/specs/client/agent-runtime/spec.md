# Spec Delta

## ADDED Requirements

### Requirement: Tool Result Previews
When the runtime gives the model a preview instead of a full tool result (Large Tool Results), the preview SHALL contain the start and the end of the result, about half each, separated by a marker on its own line that states how many bytes were left out and how to read them from the attachment. Each cut SHALL fall on a line break when one is near, and never inside a UTF-8 character.

#### Scenario: Command output with the error at the end
- **WHEN** shell.run returns 2 MB of output whose last lines are an error message and the exit summary
- **THEN** the preview the model receives contains the first lines of the output, a marker stating how many bytes were left out and how to read them, and the last lines including the error message

#### Scenario: One long line
- **WHEN** a tool returns 500 KB of JSON on a single line
- **THEN** the preview contains its start and its end, each cut at a UTF-8 character boundary, with the marker between them

### Requirement: Tool Result Cut Without Attachment
When a tool result exceeds the cap of Large Tool Results and its attachment cannot be stored, the runtime SHALL insert the start and the end of the result within the cap, cut as in Tool Result Previews, with a marker that states that the rest could not be stored.

#### Scenario: Attachment cannot be stored
- **WHEN** a result exceeds the cap and storing the attachment fails
- **THEN** the model receives the start and the end of the result within the cap, with a marker stating that the rest could not be stored
