# Spec Delta

## ADDED Requirements

### Requirement: Generation Speed Under Answers
The Thread feed SHALL show a generation speed in tokens per second under each answer of a top-level Run, also when work details are hidden. While the answer streams it SHALL show an estimate marked as approximate; when the Run's model calls have ended it SHALL show the Run's output tokens divided by the time its model calls spent generating, excluding waiting before the first token and Tool execution. The Run's detailed token usage remains a work detail.

#### Scenario: Streaming answer
- **WHEN** an answer has been streaming for two seconds with work details hidden
- **THEN** a line under it shows an approximate speed such as "≈ 38 t/s" and updates while text arrives

#### Scenario: Finished Run with Tool calls
- **WHEN** a Run calls the model twice with a 20-second Tool call between the calls and then finishes
- **THEN** the line shows the measured speed of the two model calls, unaffected by the Tool call, and no prompt or completion counts are shown unless work details are shown

#### Scenario: Waiting for the first token
- **WHEN** a model call has been running for 4 seconds without output
- **THEN** the line shows that the Run is waiting for the model and for how long

#### Scenario: Failed before output
- **WHEN** a Run fails before the model produced any output
- **THEN** no speed is shown for that Run
