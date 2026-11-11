# Spec Delta

## MODIFIED Requirements

### Requirement: Backend Failover
The gateway SHALL support several upstream backends per mapped model. It sends requests only to healthy backends. It retries a failed non-streamed request, or a stream that failed before its first upstream byte was forwarded, on another healthy backend. Keep-alive comments are not upstream bytes. A stream that is cut after its first upstream byte was forwarded MUST NOT be retried. It ends without the `[DONE]` marker, so that the client detects the truncation (client/model-access Truncated Stream Detection), and its audit outcome is "truncated".

#### Scenario: Backend down
- **WHEN** one of two backends for a model fails its health check
- **THEN** all requests for that model go to the remaining backend until the failed one recovers

#### Scenario: Stream fails before the first byte
- **WHEN** a backend fails before it sends the first byte of a streamed response, while the gateway has already sent keep-alives to the client
- **THEN** the gateway retries the request on another healthy backend, and the client sees one normal stream

#### Scenario: Stream cut mid-way
- **WHEN** a backend connection drops after the first byte of a streamed response
- **THEN** the stream to the client ends without the `[DONE]` marker, and the audit entry has the outcome "truncated"

## ADDED Requirements

### Requirement: Stream Keep-Alive
For a streamed chat request that passes the gateway's checks, the gateway SHALL send the success status and stream headers as soon as it has acquired a healthy backend, before the backend answers. It SHALL then send an SSE comment line whenever no data has been sent to the client for the keep-alive interval (server setting, default 15 seconds).

#### Scenario: Long prefill
- **WHEN** the backend takes 70 seconds to produce the first token
- **THEN** the client receives the headers at once and a keep-alive comment at least every 15 seconds until the first token

#### Scenario: Request refused before streaming
- **WHEN** a streamed request is over the user's budget
- **THEN** the gateway answers with the error status and body, without sending stream headers

### Requirement: Stream Error Event
A failure after the stream headers were sent SHALL be delivered as one stream event with an `error` object. The object carries the code, message, retry delay and reason that the error response would have carried (Backend Error Mapping). The stream then ends without the `[DONE]` marker.

#### Scenario: All backends fail after headers
- **WHEN** every healthy backend fails before its first byte, after the headers were sent
- **THEN** the client receives an error event with code MODEL_UNAVAILABLE, and the stream ends without `[DONE]`

### Requirement: Stream Time Limits
The gateway SHALL limit a streamed chat request only by three server settings: the time to the first upstream byte (default 180 seconds), the idle time between upstream chunks (default 120 seconds), and a maximum stream duration (default 20 minutes). A first-byte limit counts as a failure before the first byte (Backend Failover) and ends in TIMEOUT. An idle or maximum limit after the first byte cuts the stream.

#### Scenario: Long answer
- **WHEN** a reasoning model streams an answer for 7 minutes without pausing longer than the idle limit
- **THEN** the stream completes normally

#### Scenario: No first token
- **WHEN** the only backend sends no byte within the first-byte limit
- **THEN** the client receives an error event with code TIMEOUT

#### Scenario: Another backend after a slow one
- **WHEN** the first of two backends sends no byte within the first-byte limit
- **THEN** the gateway sends the request to the second backend, and the client keeps receiving keep-alives

#### Scenario: Stall after the first token
- **WHEN** a backend stops sending for longer than the idle limit after the first token
- **THEN** the stream ends without `[DONE]`, and the audit outcome is "truncated"

### Requirement: Prompt Cache Audit
The gateway SHALL record in the usage audit of each chat request the time from sending it upstream to the first upstream byte. For a non-streamed request, which has no separate first byte, the time to the upstream response headers is recorded instead. It SHALL also record the number of prompt tokens the upstream reports as served from its cache, or no value when the upstream reports none.

#### Scenario: Cached prefix reported
- **WHEN** the upstream reports 41,000 of 43,000 prompt tokens as cached
- **THEN** the audit entry records 41,000 cached tokens and the time to the first upstream byte
