# Spec Delta

## ADDED Requirements

### Requirement: Run Limit Note
When a Run ended at a limit (client/agent-runtime Run Limits), the Thread feed SHALL show a note under its answer that names the limit in plain words and says that replying continues the work, for example "Stopped after 200 tool calls. Reply to continue." The note SHALL be shown whatever Work Details Visibility hides.

#### Scenario: Run stopped at the tool call limit
- **WHEN** a Run ends succeeded after reaching its tool call limit
- **THEN** the feed shows its answer followed by the note "Stopped after 200 tool calls. Reply to continue."

#### Scenario: Run without a limit
- **WHEN** a Run ends succeeded with a final answer before any limit
- **THEN** no limit note is shown
