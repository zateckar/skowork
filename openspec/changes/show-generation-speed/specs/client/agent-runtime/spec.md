# Spec Delta

## ADDED Requirements

### Requirement: Model Call Timing
The runtime SHALL store one Run event at the end of every successful model call of a Run, before the answer it produced, with the time the request started, the time its first output token arrived, the time it ended, its output token count, and whether that count was reported by the provider. Usage totals remain owned by client/model-access.

#### Scenario: Streamed answer with usage
- **WHEN** a model call streams an answer and the provider reports 600 completion tokens
- **THEN** the Run has a model call event with 600 output tokens marked as reported, and its first-token time is the arrival of the first answer or reasoning text

#### Scenario: Provider omits usage
- **WHEN** a model call ends without usage from the provider
- **THEN** its model call event holds an estimated output token count marked as not reported

#### Scenario: Retried attempt
- **WHEN** a model call's first attempt streams text, fails and is retried
- **THEN** the model call event's times describe only the attempt that produced the answer

#### Scenario: Tool calls only
- **WHEN** a model call returns only tool calls without streamed text or reasoning
- **THEN** its model call event has no first-token time
