# Spec Delta

## MODIFIED Requirements

### Requirement: Streaming Responses
The client SHALL request every chat completion as a stream, including the summarizer requests of compaction. It SHALL pass text and tool-call deltas to their caller as they arrive, so the UI can show progress while the model is generating. SSE comment lines, such as the gateway's keep-alives (control-plane/llm-gateway Stream Keep-Alive), count as received data for the idle timeout and carry no content.

#### Scenario: Streaming text
- **WHEN** the model generates a long answer
- **THEN** the first text is passed on as soon as the first chunk arrives, before the response completes

#### Scenario: Summary request streamed
- **WHEN** client/agent-runtime compacts a Thread
- **THEN** the summarizer request is sent as a stream, and the keep-alives and deltas keep it within the idle timeout while the summary is generated

#### Scenario: Keep-alive during a long prefill
- **WHEN** the gateway sends only keep-alive comments for 90 seconds before the first token
- **THEN** the request is not aborted, and no text is passed to the caller until the first token arrives

#### Scenario: Small chunks are not held back
- **WHEN** the stream delivers only small chunks, such as 14-byte keep-alives or single-token deltas, with gaps of several seconds
- **THEN** each chunk is handed to the stream parser when it arrives, not when a read buffer fills or the response ends

### Requirement: Retries With Backoff
The client SHALL retry requests that fail with RATE_LIMIT or MODEL_UNAVAILABLE up to 3 times, and requests that fail with TIMEOUT once. Retries use jittered exponential backoff and honour any retry delay the gateway sends. MODEL_UNAVAILABLE with reason "role not granted" is not retried. AUTH_EXPIRED triggers one token refresh and then one retry. Other errors are not retried.

#### Scenario: Transient outage
- **WHEN** the first request fails with MODEL_UNAVAILABLE and the second succeeds
- **THEN** the Run continues without showing an error, and the retry is recorded in the Activity log

#### Scenario: Repeated timeout
- **WHEN** a request fails with TIMEOUT and its retry fails with TIMEOUT too
- **THEN** the request fails with TIMEOUT after two attempts, and the Run reports it

### Requirement: Request Timeouts
The client SHALL apply a connect timeout and an idle timeout to every model request:
- policy.llm.timeouts.connect_seconds (default 10);
- policy.llm.timeouts.idle_seconds (default 60).

The idle timeout covers the wait for the response headers and every gap between received data. The client reads both keys from the effective policy, and a changed value applies to the next request. It aborts a request that exceeds either timeout with TIMEOUT.

#### Scenario: Stalled stream
- **WHEN** no data arrives on an open stream for longer than the idle timeout
- **THEN** the request is aborted with TIMEOUT and the retry rules apply

#### Scenario: Policy raises the idle timeout
- **WHEN** an administrator sets policy.llm.timeouts.idle_seconds to 120 and the client receives the policy
- **THEN** the next model request waits up to 120 seconds without data before it fails with TIMEOUT

## ADDED Requirements

### Requirement: Stream Errors
The client SHALL treat an `error` object received inside a stream as a failure of that request. It maps the object's code to a code of Typed Error Codes, together with its retry delay and reason, exactly as it maps an error response. Output streamed before the error is marked incomplete.

#### Scenario: Gateway times out after sending headers
- **WHEN** a stream of keep-alives ends with an error event whose code is TIMEOUT
- **THEN** the request fails with TIMEOUT, and the retry rules apply
