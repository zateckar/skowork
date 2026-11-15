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

### Requirement: Context Compaction
When the conversation would exceed the context budget of the model role in use, the runtime SHALL compact it. It first lets BeforeCompaction handlers flush durable facts to memory, then replaces older turns with a summary, keeping the system instructions, the latest user message and unresolved tool calls.

#### Scenario: Long run
- **WHEN** a Run's next request would exceed 85% of the model's context window
- **THEN** the runtime compacts older turns into a summary before sending the request, and records a compaction event in the Thread's Activity log

### Requirement: Generated Surface Instructions
The runtime SHALL tell the model the supported versioned surface contract and component catalog for Thread answers, including the requirement to retain a readable answer when no surface is usable.

#### Scenario: Model composes a response
- **WHEN** the runtime prepares a model request for a Thread with surface rendering available
- **THEN** the model receives the supported surface contract and can choose a suitable composition for the response

### Requirement: Incomplete Generated Response Fallback
When surface rendering is enabled and a model request ends unsuccessfully or is cancelled after emitting text, the runtime SHALL retain bounded incomplete text as a display-only Run event. It MUST NOT treat that text as a completed assistant answer or include it in subsequent model conversation history. A retry discards the failed attempt's partial text before capturing the next attempt.

#### Scenario: A user stops an incomplete response
- **WHEN** a user cancels a Run after a model has streamed part of a response
- **THEN** the emitted text remains readable in the Thread after reopening, and no surface in it is interactive

### Requirement: Structured Question Response
The runtime SHALL accept a structured response to an active generated Question only when it matches the pending Question, its Run and surface revision, and SHALL return the visible user-selected values to the asking Run as its tool result. Existing free-text answers remain available.

#### Scenario: Answer a pending Question
- **WHEN** the user submits a valid structured response for an active generated Question
- **THEN** the asking Run resumes with the submitted values and the Question is resolved

#### Scenario: Stale response
- **WHEN** a response targets a Question already resolved by a different response, or a superseded surface revision
- **THEN** the runtime rejects it without resuming a Run or executing a tool

#### Scenario: Malformed structured form
- **WHEN** an `ask_user` form is invalid or contains other than one submit button
- **THEN** the Tool reports the invalid form and creates no active generated controls

### Requirement: Generated Surface Follow-up
The runtime SHALL accept an explicit submission from a completed assistant answer as a user message in the same Thread. If another Run is active, the submission SHALL follow client/agent-runtime Message Queueing And Steering using the user's selected mode.

#### Scenario: Continue from an answer
- **WHEN** the user submits selections from a completed answer while no Run is active
- **THEN** the values are recorded as a user message and start a Run in that Thread

### Requirement: Durable Surface Submissions
The runtime SHALL durably bind a surface submission to its Thread, Run, surface and revision and accept it at most once, including after restart. A submitted value MUST be derived from the user's visible controls.

#### Scenario: Retry after restart
- **WHEN** the client retries a surface submission already accepted before a restart
- **THEN** the existing result is returned and the Run is not resumed or started a second time

### Requirement: Generated Surface Tool Requests
The runtime SHALL treat a surface submission as user input and evaluate any resulting tool call through client/policy-enforcement. A generated surface MUST NOT resolve an Approval Action item.

Internal generated-surface drafts and acceptance records MUST NOT be exposed as readable Thread attachments to Tools or as user attachments. Their storage MUST remain protected from plaintext disclosure when opened by an older client.

#### Scenario: Surface requests sending mail
- **WHEN** a submitted surface asks the agent to send mail and the tool call requires approval
- **THEN** the Run waits for the existing Approval Action item before the tool can execute
