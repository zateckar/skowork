# Spec Delta

## ADDED Requirements

### Requirement: Reasoning Effort Setting
A platform-admin SHALL be able to store a reasoning effort for each chat model of an LLM connection: one of `none`, `minimal`, `low`, `medium`, `high`, `xhigh` and `max`, or none (the model's own default). The gateway sends the stored value as `reasoning_effort` in every chat request routed to that model, replacing any value in the client's request. A model without a stored value receives the client's request unchanged.

#### Scenario: Effort applied
- **WHEN** the model mapped to `main` has the reasoning effort `low` and a client sends a chat request for `main`
- **THEN** the upstream receives `reasoning_effort: "low"`

#### Scenario: No effort stored
- **WHEN** the model has no stored reasoning effort
- **THEN** the forwarded request contains no `reasoning_effort` unless the client sent one

### Requirement: Reasoning Effort Check
Before it stores a reasoning effort, the gateway SHALL check the value with a one-token chat request to an enabled connection that offers the model. Clearing the value needs no check.

#### Scenario: Unsupported value
- **WHEN** a platform-admin sets the reasoning effort `high` on a model whose upstream accepts only `low`, `medium` and `xhigh`
- **THEN** the value is not stored, and the admin sees that the model rejected `high`, with the values the upstream named without credentials or upstream addresses

#### Scenario: Value cannot be checked
- **WHEN** the upstream answers 503 or cannot be reached while the value is checked
- **THEN** the value is not stored, and the admin sees that it could not be checked

### Requirement: Reasoning Effort In Connection Test
Testing an LLM connection SHALL also check the stored reasoning effort of each of its chat models in the same way, and report the models that reject theirs.

#### Scenario: Test finds a stale value
- **WHEN** a connection is tested and the upstream now rejects a model's stored effort
- **THEN** the test result names that model and value as failing

### Requirement: Fallback Model Setting
A platform-admin SHALL be able to store, for each chat model M, a fallback model F and a fallback wait (5 to 600 seconds, default 60). F MUST resolve to a known chat model other than M that has no fallback of its own, and M MUST NOT be the fallback of another model; any other value is refused with INVALID_REQUEST.

#### Scenario: Fallback chain refused
- **WHEN** a platform-admin sets F as the fallback of M while F has a fallback of its own
- **THEN** the setting is refused, and M keeps its previous fallback

### Requirement: Model Fallback
The gateway SHALL send a chat request for model M to M's fallback model F when every attempt on M ended before M's first byte because M's fallback wait passed, or with TIMEOUT, MODEL_UNAVAILABLE, an upstream 429 or all backends at capacity. It does so only if the request fits F. Otherwise the original failure stands.

#### Scenario: Primary too slow
- **WHEN** M has fallback F with a wait of 60 seconds, and M sends no first byte within 60 seconds
- **THEN** the gateway sends the request to F, and the client receives F's answer in the same response

#### Scenario: Fallback on an unhealthy connection
- **WHEN** M's only connection is marked unhealthy by M's own 503 and F is served by the same connection
- **THEN** the request is still sent to F on that connection

#### Scenario: Request does not fit the fallback
- **WHEN** M times out on a request of 100,000 estimated tokens and F's context window is 65,536
- **THEN** the request fails with TIMEOUT, and nothing is sent to F

#### Scenario: Error that does not fall back
- **WHEN** M rejects a request with CONTEXT_LENGTH or INVALID_REQUEST
- **THEN** the client receives that error, and nothing is sent to F

### Requirement: Fallback Request
The request sent to a fallback model SHALL be the client's request with the fallback's model id and the fallback's reasoning effort, or without `reasoning_effort` when the fallback has none stored. It uses the normal stream time limits (Stream Time Limits). It counts against the user's chat budget like any chat request. A failure of the fallback is returned to the client with its own error code.

#### Scenario: Fallback without an effort
- **WHEN** M has the effort `low`, F has none stored, and a request falls back to F
- **THEN** the request to F contains no `reasoning_effort`

### Requirement: Fallback Cooldown
After a request for model M falls back to F, the gateway SHALL send new requests for M that F fits directly to F, without trying M, for a cooldown period (server setting, default 2 minutes). The first request after the cooldown tries M again. A request that F does not fit tries M even during the cooldown.

#### Scenario: Outage of the primary
- **WHEN** a request for M fell back to F, and another request for M arrives 30 seconds later
- **THEN** that request goes to F immediately, and its audit entry records the fallback reason `cooldown`

### Requirement: Fallback Audit
The usage audit entry of each chat request SHALL record the model that produced the response and the fallback reason: none, `timeout`, `unavailable`, `capacity` or `cooldown`. The mapped model stays recorded as before.

#### Scenario: Audited fallback
- **WHEN** a request for M is answered by F after M timed out
- **THEN** its audit entry names M as the mapped model, F as the model that answered, and `timeout` as the fallback reason
