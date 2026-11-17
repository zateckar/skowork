# Spec Delta

## ADDED Requirements

### Requirement: Output Limit Continuation
A model response that stopped at the role's output limit (finish reason `length`) without a valid tool call SHALL NOT end the Run as succeeded. The runtime SHALL keep it in the Thread and request a continuation with a request-only note asking for short reasoning and long content split over several tool calls; the note MUST NOT be stored. After two consecutive continuations, a third such response SHALL fail the Run with the error code `output_limit`.

#### Scenario: Reasoning uses the whole limit
- **WHEN** the model's response stops at the output limit with reasoning only, no answer text and no tool call
- **THEN** the Run is not succeeded and the next model request asks the model to continue

#### Scenario: Draft carried into the continuation
- **WHEN** the cut-off response drafted content in its reasoning
- **THEN** the continuation note carries at most the last 8000 characters of that reasoning and text, and later requests do not

#### Scenario: Continuation finishes the work
- **WHEN** the continuation request returns tool calls or a finished answer
- **THEN** the Run proceeds as usual and later requests do not contain the continuation note

#### Scenario: Limit reached again and again
- **WHEN** three consecutive responses of a Run stop at the output limit without a valid tool call
- **THEN** the Run fails with the error code `output_limit`, a reason that names the limit, and any partial answer kept
