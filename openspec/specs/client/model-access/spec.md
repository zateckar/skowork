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
