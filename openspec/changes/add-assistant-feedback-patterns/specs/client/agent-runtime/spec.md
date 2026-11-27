# Spec Delta

## ADDED Requirements

### Requirement: Run Plan Tool
The runtime SHALL provide an `update_plan` tool to top-level Runs as part of the core set of Lazy Tool Loading. Its arguments are the complete Run plan: 1 to 20 steps in order, each with a title of at most 200 characters and a status of pending, running, done, failed or skipped, with at most one step running. Each call replaces the Run's previous Run plan. The tool declares the Risk class read, has no effect outside the Run, and is never offered to Child Runs. Invalid arguments return an error result naming the problem and leave the previous Run plan in force. The system prompt asks the model to publish a Run plan for work of three or more steps and to update it as steps finish.

#### Scenario: Plan published
- **WHEN** the model calls `update_plan` with three steps, the first running and the others pending
- **THEN** the call succeeds without approval, and the stored tool call holds those three steps as the Run's current Run plan

#### Scenario: Plan updated
- **WHEN** the model later calls `update_plan` with the first step done and the second running
- **THEN** that call's steps replace the earlier Run plan

#### Scenario: Two running steps
- **WHEN** the model calls `update_plan` with two steps marked running
- **THEN** the call returns an error result saying that at most one step may be running, and the previous Run plan stays in force

#### Scenario: Subagent
- **WHEN** a Child Run's model asks for its tools
- **THEN** `update_plan` is not among them

### Requirement: Reasoning Duration
When a model call streamed reasoning, the runtime SHALL store with that reasoning the time in milliseconds from its first to its last streamed reasoning text. Reasoning that was not streamed is stored without a duration.

#### Scenario: Streamed reasoning
- **WHEN** a model call streams reasoning for 8 seconds and then answers
- **THEN** the stored reasoning carries a duration of about 8000 milliseconds

#### Scenario: Reasoning without stream
- **WHEN** the provider returns reasoning only with the complete response
- **THEN** the stored reasoning has no duration
