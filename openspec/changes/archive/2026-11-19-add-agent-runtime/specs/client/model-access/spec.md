# Spec Delta

## Purpose

Defines how the client calls the on-prem models. Every call (chat, embeddings, reranking) goes through the control-plane gateway and names a model role. The capability also covers streaming, reasoning output, robust handling of imperfect tool calls and truncated streams, typed error codes, retries, image input and usage accounting.

## ADDED Requirements

### Requirement: Gateway-Only Model Access
The client SHALL send every model request to control-plane/llm-gateway, authenticated with the user's access token from client/identity. It MUST NOT connect to model servers directly or store any model credential.

#### Scenario: Request authentication
- **WHEN** the runtime makes a model call
- **THEN** the request goes to the gateway URL that the control plane returned at registration (client/control-channel), and carries the user's access token, whose audience is the control plane's configured audience (control-plane/identity Identity Provider Trust)

### Requirement: Model Roles
The client SHALL address models only by role:
- `main`, for agent reasoning and tool use;
- `vision`, for requests with images;
- `small`, for titles, summaries and extraction;
- `embed`, for text embeddings;
- `rerank`, for reranking candidate passages.

The gateway resolves each role to an upstream model. The client discovers each role's context window and capabilities from the gateway's model list.

#### Scenario: Image request uses vision role
- **WHEN** a request contains an image
- **THEN** it is sent with the `vision` role, and the context budget used for it is that role's reported context window

#### Scenario: Images never go to other roles
- **WHEN** a feature tries to send an image with any role other than `vision`
- **THEN** the client refuses the request before sending it

### Requirement: Embedding Requests
The client SHALL request embeddings for text batches with the `embed` role, using the gateway's embeddings endpoint. It returns each vector together with the upstream model identifier and dimension reported by the gateway, so callers can detect a model change.

#### Scenario: Embed a batch
- **WHEN** client/knowledge-graph submits 16 chunks for embedding
- **THEN** one request with role `embed` is sent, and 16 vectors are returned with the model identifier and dimension

### Requirement: Rerank Requests
The client SHALL rerank candidate passages for a query with the `rerank` role, using the gateway's rerank endpoint. It returns a relevance score for each passage in the input order.

#### Scenario: Rerank candidates
- **WHEN** retrieval sends a query and 30 candidate passages for reranking
- **THEN** the client returns 30 relevance scores, one for each passage

### Requirement: Reasoning Output
When a model returns reasoning content separately from its answer, the client SHALL pass it to the runtime as reasoning, distinct from answer text, including streamed reasoning deltas. Reasoning MUST NOT be resent to the model in later turns, and reasoning tokens count towards usage.

#### Scenario: Reasoning model answers
- **WHEN** the `main` model streams reasoning deltas before its answer
- **THEN** the reasoning is delivered as reasoning and shown collapsed by client/work-inbox, the answer is delivered as text, and the next turn's request contains only the answer

### Requirement: Streaming Responses
The client SHALL request streamed responses and pass text and tool-call deltas to the runtime as they arrive, so the UI can show progress while the model is generating.

#### Scenario: Streaming text
- **WHEN** the model generates a long answer
- **THEN** the first text is passed on as soon as the first chunk arrives, before the response completes

### Requirement: Truncated Stream Detection
The client SHALL treat a stream that ends without a finish reason or terminal marker as a failed request with error MODEL_UNAVAILABLE. MODEL_UNAVAILABLE is retryable. The client MUST NOT treat such a stream as a completed answer.

#### Scenario: Connection drops mid-stream
- **WHEN** the connection closes after partial output and no finish reason has arrived
- **THEN** the request is reported as failed, the partial output is marked incomplete, and the retry rules apply

### Requirement: Tool Call Validation And Repair
The client SHALL parse every tool call's arguments and validate them against the tool's schema. Malformed JSON is repaired when the repair is unambiguous. If validation still fails, the model is asked once to correct the call before the error is returned to the Run as a tool result.

#### Scenario: Trailing comma in arguments
- **WHEN** the model emits tool arguments with a trailing comma
- **THEN** the arguments are repaired and validated without an extra model call

#### Scenario: Persistently invalid call
- **WHEN** the corrected call still fails schema validation
- **THEN** the tool is not executed, and the model receives a validation error as the tool result

### Requirement: Typed Error Codes
The client SHALL map every failed model request to exactly one error code and pass along any detail (reset time, retry delay, reason):
- AUTH_EXPIRED: the token was rejected.
- RATE_LIMIT: the request was over a rate limit.
- QUOTA_EXCEEDED: the budget is exhausted.
- CONTEXT_LENGTH: the request is too large.
- MODEL_UNAVAILABLE: the backend is down, the stream was truncated, or the role is not granted.
- TIMEOUT: a timeout elapsed.

client/work-inbox owns the user-facing remedies.

#### Scenario: Quota exhausted
- **WHEN** the gateway rejects a request because the user's token budget is exhausted
- **THEN** the Run fails with QUOTA_EXCEEDED, and the reset time is passed to the runtime

#### Scenario: Role not granted
- **WHEN** the gateway refuses a request because the user's policy maps no model to the requested role
- **THEN** the request fails with MODEL_UNAVAILABLE, the reason is "role not granted", and it is not retried

### Requirement: Retries With Backoff
The client SHALL retry requests that fail with RATE_LIMIT, MODEL_UNAVAILABLE or TIMEOUT up to 3 times, using jittered exponential backoff and honouring any retry delay the gateway sends. MODEL_UNAVAILABLE with reason "role not granted" is not retried. AUTH_EXPIRED triggers one token refresh and then one retry. Other errors are not retried.

#### Scenario: Transient outage
- **WHEN** the first request fails with MODEL_UNAVAILABLE and the second succeeds
- **THEN** the Run continues without showing an error, and the retry is recorded in the Activity log

### Requirement: Image Input
The client SHALL downscale images before sending them, so they fit the vision role's limits: longest side 1568 px and at most 5 MB per image by default. It MUST NOT send more images in one request than policy.llm.max_images_per_request (integer, default 4). Tools that send images, such as browser and computer use, rely on this requirement.

#### Scenario: Large screenshot
- **WHEN** a 3840×2160 screenshot is attached to a request
- **THEN** it is downscaled to fit the limits before sending, preserving aspect ratio

### Requirement: Usage Accounting
The client SHALL record the prompt, completion and reasoning token counts reported for every model request, attribute them to the Run, Thread and model role, and show the totals per Run in the Activity log. Embedding and rerank requests made outside a Run are attributed to the feature that made them.

#### Scenario: Usage shown for a run
- **WHEN** a Run finishes
- **THEN** its Activity log shows the total prompt and completion tokens used, and the number of model calls

### Requirement: Request Timeouts
The client SHALL apply a connect timeout and an idle-stream timeout to every model request:
- policy.llm.timeouts.connect_seconds (default 10);
- policy.llm.timeouts.idle_seconds (default 60).

It aborts requests that exceed either one with TIMEOUT.

#### Scenario: Stalled stream
- **WHEN** no data arrives on an open stream for longer than the idle timeout
- **THEN** the request is aborted with TIMEOUT and the retry rules apply
