# Spec Delta

## ADDED Requirements

### Requirement: Run Event Stream
Every event the runtime stores in a Run's event log SHALL also be sent to the runtime's listeners once, with the sequence number it was stored under. This includes reasoning blocks, tool calls and state changes. A listener therefore sees a gap in the sequence only when an event is really missing.

#### Scenario: Reasoning then answer
- **WHEN** a model response carries reasoning and an answer
- **THEN** the listeners receive the reasoning event and then the message event, with consecutive sequence numbers

### Requirement: Knowledge Citation Markers
The stable system prompt SHALL tell the agent that, when an answer uses a passage returned by knowledge.search, it writes that passage's number in square brackets after the sentence it supports, such as [2]. The agent SHALL NOT write the number of a passage it did not use. The instruction does not change within a Thread (Stable Prompt Prefix).

#### Scenario: Cited passage
- **WHEN** the agent answers from passage 2 of a search
- **THEN** the answer contains [2] after the sentence that passage supports

#### Scenario: Unused passage
- **WHEN** passage 3 was returned but the agent does not use it
- **THEN** the answer contains no [3]
