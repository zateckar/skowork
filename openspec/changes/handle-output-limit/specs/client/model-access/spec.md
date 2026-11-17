# Spec Delta

## ADDED Requirements

### Requirement: Tool Calls Cut Off At The Output Limit
In a response that stopped at the output limit (finish reason `length`), a tool call whose arguments were valid only after repair SHALL be treated as invalid, with an error saying that it was cut off at the output limit and that the model should send shorter arguments. It then follows Tool Call Validation And Repair. Tool calls whose arguments were complete without repair remain valid.

#### Scenario: File body cut off
- **WHEN** a response stops at the output limit inside the content argument of a `files.write` call and repair would close the JSON
- **THEN** the call is invalid, it does not run, and the model is asked for a corrected call

#### Scenario: Complete call before the cut
- **WHEN** a response that stops at the output limit contains a complete tool call followed by a cut-off one
- **THEN** the complete call is valid and only the cut-off call is invalid
