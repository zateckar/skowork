# Spec Delta

## ADDED Requirements

### Requirement: Tool Call Size Instructions
The system prompt of every Run and subagent Run SHALL tell the model to keep the arguments of each tool call under about 6,000 tokens. The instruction states the bound also as about 20,000 characters and roughly 400 lines of code. It SHALL tell the model to split larger content over several tool calls, by writing several smaller files or by appending further parts with files.write. The instruction is the same text in every Run.

#### Scenario: Large Local app
- **WHEN** a model call of any Run or subagent Run is sent
- **THEN** its system prompt contains the tool call size instruction, including how to split content with files.write append

#### Scenario: Prompt stays stable
- **WHEN** two Runs of the same Thread start at different times
- **THEN** both system prompts contain the same tool call size instruction, byte for byte
