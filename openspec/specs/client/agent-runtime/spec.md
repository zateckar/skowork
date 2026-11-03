# client/agent-runtime Specification

## Purpose
How the client executes a Run: the model loop, tool calls and their results, pausing and resuming, and provenance of untrusted content. The full capability arrives with add-agent-runtime; this spec holds the requirements already merged from later changes.

## Requirements

### Requirement: Image Tool Results
A tool result MAY carry images, such as a browser screenshot. The runtime SHALL send them to the model only in the next model request of the Run, which then uses the `vision` role and the image input rules of client/model-access. Image bytes MUST NOT be stored in the Thread: the stored tool result records only that an image was returned, and the image is dropped from memory after that request.

#### Scenario: Screenshot reaches the model
- **WHEN** a tool returns a screenshot as its result
- **THEN** the next model request of the Run carries the image with the `vision` role, and later requests do not carry it

#### Scenario: Image not persisted
- **WHEN** a Run that received an image tool result ends
- **THEN** the Thread's stored events contain the tool result text and a note that an image was returned, but no image data
