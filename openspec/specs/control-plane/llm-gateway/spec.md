# control-plane/llm-gateway Specification

## Purpose
The control-plane gateway every model call goes through: its endpoints, the mapping of model roles to upstream models, and per-user budgets.

## Requirements

### Requirement: Image Generation Endpoint
The gateway SHALL expose an OpenAI-compatible image generation endpoint that accepts only the `image` role as its model. It forwards the prompt, size and count to the upstream model mapped to `image` and returns the generated images as base64 data. It MUST refuse any other role on this endpoint, and the `image` role on every other endpoint, with 400 INVALID_REQUEST.

#### Scenario: Generate an image
- **WHEN** a user whose policy grants the `image` role requests one image
- **THEN** the gateway forwards the request to the upstream model mapped to `image` and returns the image as base64 data

#### Scenario: Image role on chat completions
- **WHEN** a client sends a chat completion request with model "image"
- **THEN** the gateway refuses it with 400 INVALID_REQUEST, and no backend call is made

### Requirement: Image Role Mapping
policy.llm.models SHALL also map the role `image`, with the default `image` → acme-llm-image. Like every role, a Group can remove it, and a user without it is refused with 403 MODEL_UNAVAILABLE and the reason "role not granted". The model list shows the `image` role with `kind` "image" to users who have it.

#### Scenario: Group without image generation
- **WHEN** a user's effective policy.llm.models has no `image` entry and the user requests an image
- **THEN** the gateway refuses with 403 MODEL_UNAVAILABLE and the reason "role not granted"

### Requirement: Image Budget
The gateway SHALL limit each user to policy.llm.budget.images_daily generated images per day (integer, default 50). It checks the budget before forwarding: a request whose image count would exceed the remaining budget is rejected with QUOTA_EXCEEDED and the reset time. Image requests MUST NOT count against the chat or embed token budgets.

#### Scenario: Image budget used up
- **WHEN** a user who has generated 50 images today requests another one under the default budget
- **THEN** the gateway rejects it with QUOTA_EXCEEDED and the time the budget resets, and no backend call is made

#### Scenario: Chat budget unaffected
- **WHEN** a user generates 10 images in a day
- **THEN** that user's chat token budget for the day is unchanged

### Requirement: Server Key From A Passphrase
The control plane SHALL derive the server key that encrypts LLM connection credentials from SKOWORK_SECRET_KEY: a value that is the base64 encoding of 32 bytes is the key itself, and any other value of at least 16 characters is a passphrase from which the key is derived deterministically. A shorter value MUST stop the control plane at startup with a message naming SKOWORK_SECRET_KEY, without a stack trace.

#### Scenario: Passphrase typed by an admin
- **WHEN** SKOWORK_SECRET_KEY is set to a passphrase such as `correct horse battery staple` and an LLM connection is added
- **THEN** the control plane starts, the credential is stored encrypted, and after a restart with the same passphrase the connection still works

#### Scenario: Existing base64 key
- **WHEN** SKOWORK_SECRET_KEY holds the base64 encoding of 32 bytes that a deployment already used
- **THEN** credentials stored under it are still decrypted

#### Scenario: Too short
- **WHEN** SKOWORK_SECRET_KEY is `secret`
- **THEN** the control plane exits with `refusing to start: SKOWORK_SECRET_KEY ...` and no stack trace

### Requirement: Sampling Defaults Setting
A platform-admin SHALL be able to store sampling defaults for each chat model: `temperature` (0 to 2), `top_p` (above 0 to 1), `top_k` (-1 or 1 to 1000), `min_p` (0 to 1), `presence_penalty` (-2 to 2) and `repetition_penalty` (above 0 to 2), each optional; other keys or values are refused with INVALID_REQUEST. The gateway SHALL add each stored value to a chat request routed to that model when the client's request does not contain that field. When a fallback model answers, its own defaults apply.

#### Scenario: Defaults applied
- **WHEN** the model mapped to `main` stores `temperature` 1.0 and `top_k` 20 and a client sends a chat request for `main` without sampling fields
- **THEN** the upstream receives `temperature: 1.0` and `top_k: 20`

#### Scenario: Client value kept
- **WHEN** the client's request contains `temperature: 0`
- **THEN** the upstream receives `temperature: 0` and the other stored defaults

#### Scenario: Out of range
- **WHEN** a platform-admin stores `top_p` 1.5
- **THEN** the setting is refused with INVALID_REQUEST and the previous defaults stay

### Requirement: Keep-Alive Between Events
The gateway SHALL write a keep-alive comment to a streamed response only before the first upstream byte, or after a complete upstream event (an event that ends with a blank line). It MUST NOT write any bytes of its own inside an upstream event.

#### Scenario: Upstream stalls inside an event
- **WHEN** the upstream sends half of a `data:` line and then sends nothing for longer than the keep-alive interval
- **THEN** no keep-alive is written until the event is complete, and the client receives the event unchanged

#### Scenario: Upstream idle between events
- **WHEN** the upstream sends nothing for longer than the keep-alive interval after a complete event
- **THEN** the gateway writes a keep-alive comment
