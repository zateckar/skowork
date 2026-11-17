# Spec Delta

## ADDED Requirements

### Requirement: Malformed Stream Data
A streamed chat response whose SSE `data` payload is neither `[DONE]` nor a JSON object SHALL fail as a retryable malformed response. The client MUST NOT skip the payload and continue. The failure reason SHALL name a malformed stream event and its size, without its content. SSE comments are not data and stay ignored.

#### Scenario: Damaged event
- **WHEN** a stream contains a `data` payload that is cut off JSON
- **THEN** the response fails as malformed and no further deltas are added to the output

#### Scenario: Comment line
- **WHEN** a stream contains a `: keep-alive` comment between events
- **THEN** the comment is ignored and the response continues
