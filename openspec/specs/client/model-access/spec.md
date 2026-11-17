# client/model-access Specification

## Purpose
How the client calls models through the control-plane LLM gateway by model role, and what model errors mean.

## Requirements

### Requirement: Image Generation Requests
The client SHALL request generated images only with the role `image`, through the gateway's image generation endpoint, with a text prompt, a size and a count. It MUST NOT send the `image` role to any other endpoint. Cancelling the Run that made the request aborts it.

#### Scenario: Image request uses the image role
- **WHEN** a tool requests one generated image
- **THEN** one request with role `image` is sent to the gateway's image generation endpoint

#### Scenario: Cancelled while generating
- **WHEN** the user stops a Run while an image request is in flight
- **THEN** the client aborts the request, and no image is stored

### Requirement: Malformed Stream Data
A streamed chat response whose SSE `data` payload is neither `[DONE]` nor a JSON object SHALL fail as a retryable malformed response. The client MUST NOT skip the payload and continue. The failure reason SHALL name a malformed stream event and its size, without its content. SSE comments are not data and stay ignored.

#### Scenario: Damaged event
- **WHEN** a stream contains a `data` payload that is cut off JSON
- **THEN** the response fails as malformed and no further deltas are added to the output

#### Scenario: Comment line
- **WHEN** a stream contains a `: keep-alive` comment between events
- **THEN** the comment is ignored and the response continues
