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

### Requirement: OpenAI-Compatible Surface
The gateway SHALL expose four OpenAI-compatible endpoints to clients:
- chat completions, streamed and non-streamed, with tools and image inputs;
- a model list;
- embeddings;
- rerank.

A request names the role (main, vision, small, embed or rerank) as its model. Reasoning content from upstream is passed through unchanged.

#### Scenario: Chat completion by role
- **WHEN** a client sends a chat completion request with model "main"
- **THEN** the gateway forwards it to the upstream model mapped to "main" for the user's Groups, and streams the response back unchanged in format

#### Scenario: Embeddings by role
- **WHEN** a client sends an embeddings request with model "embed"
- **THEN** the gateway forwards it to the upstream model mapped to "embed", and returns the vectors with the upstream model identifier and, as a top-level `dimension` field, the vector dimension

### Requirement: Organization LLM API Upstream
The gateway SHALL forward requests to the LLM connections that platform-admins manage (LLM Connections). Each connection is an OpenAI-compatible endpoint, normally the organization LLM API. The gateway authenticates to it with the connection's credential, which is held only by the gateway.

#### Scenario: Upstream credential never reaches clients
- **WHEN** a client calls any gateway endpoint
- **THEN** the gateway adds the connection's credential, and neither the credential nor the upstream address appears in any response to the client

#### Scenario: Development upstream
- **WHEN** the control plane starts in its development configuration with no LLM connection stored and the environment variables OPENAI_BASE_URL and OPENAI_KEY set
- **THEN** it creates a connection named `primary` from them, and there is no fallback to a mock backend

### Requirement: Token Authentication
The gateway SHALL accept only requests that carry a valid user access token accepted under control-plane/identity Identity Provider Trust. It MUST reject all other requests without contacting any backend. A token that is not a user access token, such as a client-credentials token, is refused with 403.

#### Scenario: Missing token
- **WHEN** a request arrives without a valid token
- **THEN** the gateway responds with an authentication error, and no backend call is made

#### Scenario: Client-credentials token
- **WHEN** a request carries a client-credentials token that is not a user access token
- **THEN** the gateway refuses it with 403, and no backend call is made

### Requirement: Per-Group Role Mapping
The gateway SHALL resolve each role to an upstream model using the user's effective policy.llm.models. That key maps each role (main, vision, small, embed, rerank) to an upstream model id, and it is also the user's model allowlist. Its default is main and vision → acme-llm-primary, small → acme-llm-secondary, embed → acme-llm-embedding, rerank → acme-llm-reranker. A Group can remove a role.

#### Scenario: Mapped role
- **WHEN** a user whose policy maps `small` to an upstream model sends a request with role "small"
- **THEN** the gateway forwards it to that upstream model

### Requirement: Role Refusals
The gateway MUST refuse a request for a role that is not granted to the user with 403 and the code MODEL_UNAVAILABLE, with reason "role not granted". It MUST refuse a request that names a role that does not fit the endpoint (for example "embed" on chat completions) with 400 and the code INVALID_REQUEST.

#### Scenario: Role not granted
- **WHEN** a user whose policy maps no `vision` model sends a request with role "vision"
- **THEN** the gateway refuses it with 403 MODEL_UNAVAILABLE and the reason "role not granted"

#### Scenario: Role does not fit the endpoint
- **WHEN** a user whose policy grants the `embed` role sends a chat completion request with model "embed"
- **THEN** the gateway refuses it with 400 INVALID_REQUEST, and no backend call is made

### Requirement: Filtered Model List
The gateway SHALL return, in its model list, only the roles available to the requesting user. Each entry is a flat object with these fields:
- `id` (the role) and `kind`;
- `context_window` and `max_output_tokens`;
- `tools`, `images` and `reasoning`, which state tool support, image support and reasoning output;
- for the embed role, also `dimension`, the vector dimension.

Values that the upstream does not report come from gateway server settings.

#### Scenario: Model list for a standard user
- **WHEN** a user with only `main` and `small` roles requests the model list
- **THEN** the list contains exactly `main` and `small`, each with `id`, `kind`, `context_window`, `max_output_tokens`, `tools`, `images` and `reasoning`

#### Scenario: Embedding entry
- **WHEN** a user with the `embed` role requests the model list
- **THEN** the `embed` entry also carries `dimension`

### Requirement: Token Budgets
The gateway SHALL enforce two per-user token budgets, set per Group:
- chat roles (main, vision, small): policy.llm.budget.tokens (default 1,000,000) per policy.llm.budget.period (day | week | month, default day);
- embed and rerank roles: policy.llm.budget.embed_daily_tokens (default 12,000,000) per day.

When a budget is exhausted, requests for its roles are rejected with QUOTA_EXCEEDED and the reset time.

#### Scenario: Budget exhausted
- **WHEN** a user's daily chat token budget is used up
- **THEN** further chat requests that day are rejected with QUOTA_EXCEEDED and the time the budget resets

#### Scenario: Embedding does not consume the chat budget
- **WHEN** knowledge indexing uses 5,000,000 embedding tokens in a day
- **THEN** the user's chat budget for that day is unchanged

### Requirement: Budget Accounting
The gateway SHALL record usage after a response completes. Budgets are soft: usage is known only when a response completes, so requests of one user that are in flight together can overshoot a budget slightly. This is accepted behaviour. Rerank usage counts against the embed budget, taken from `usage.total_tokens`, else from `meta.billed_units.input_tokens`, else from an estimate of the input.

#### Scenario: Rerank counts as embedding usage
- **WHEN** a rerank response reports no `usage.total_tokens` but `meta.billed_units.input_tokens` is 800
- **THEN** 800 tokens are added to the user's embed budget and nothing to the chat budget

#### Scenario: Overshoot by in-flight requests
- **WHEN** two requests of one user are in flight while the budget has a few tokens left
- **THEN** both complete, the recorded usage may exceed the budget slightly, and the next request is rejected with QUOTA_EXCEEDED

### Requirement: Rate Limits
The gateway SHALL enforce per-user request-rate and concurrency limits from policy.llm.rate_limits:
- requests_per_minute (default 60);
- concurrent_requests (default 4).

These per-user limits are enforced in memory by the gateway process. It also enforces per-backend capacity limits from gateway server settings. Requests over a limit are rejected with RATE_LIMIT and a retry delay.

#### Scenario: Burst of requests
- **WHEN** a user exceeds the per-user concurrent-request limit
- **THEN** the excess requests are rejected with RATE_LIMIT and a retry delay

#### Scenario: All backends at capacity
- **WHEN** every healthy backend of the mapped model is at its capacity limit
- **THEN** the request is rejected with RATE_LIMIT and a retry delay

### Requirement: Usage Audit
The gateway SHALL record an audit entry for every request: user principal, installation identifier, Groups, role, backend model, token counts, latency, outcome (for example ok or truncated) and Run identifier. It MUST NOT record prompt or response content unless the user's effective policy.telemetry.level is content (client/telemetry). Embedding and rerank inputs and outputs are never recorded.

#### Scenario: Default audit
- **WHEN** a request completes for a user whose telemetry level is `metadata`
- **THEN** the audit entry contains usage metadata and no message content

#### Scenario: Content level
- **WHEN** a request completes for a user whose telemetry level is `content`
- **THEN** the audit entry of a chat request also contains the request and response content, and the audit entry of an embedding request still contains only usage metadata

### Requirement: Backend Failover
The gateway SHALL support several upstream backends per mapped model. It sends requests only to healthy backends, and retries a failed non-streamed request, or a stream that failed before its first byte, on another healthy backend. A stream that is cut after its first byte MUST NOT be retried: it ends without the `[DONE]` marker, so that the client detects the truncation (client/model-access Truncated Stream Detection), and its audit outcome is "truncated".

#### Scenario: Backend down
- **WHEN** one of two backends for a model fails its health check
- **THEN** all requests for that model go to the remaining backend until the failed one recovers

#### Scenario: Stream fails before the first byte
- **WHEN** a backend fails before it sends the first byte of a streamed response
- **THEN** the gateway retries the request on another healthy backend, and the client sees one normal stream

#### Scenario: Stream cut mid-way
- **WHEN** a backend connection drops after the first byte of a streamed response
- **THEN** the stream to the client ends without the `[DONE]` marker, and the audit entry has the outcome "truncated"

### Requirement: Backend Error Mapping
The gateway SHALL map backend failures to client error codes as follows:
- backend 401, 403, 404 and 5xx: 503 MODEL_UNAVAILABLE. AUTH_EXPIRED is never used for a backend failure; it is reserved for the user's own token;
- backend 429: RATE_LIMIT, carrying the upstream Retry-After;
- backend timeout: 504 TIMEOUT.

#### Scenario: Backend rejects the gateway credential
- **WHEN** a backend answers 401
- **THEN** the client receives 503 MODEL_UNAVAILABLE, not AUTH_EXPIRED

#### Scenario: Backend rate limit
- **WHEN** a backend answers 429 with Retry-After 12
- **THEN** the client receives RATE_LIMIT with a retry delay of 12 seconds

#### Scenario: Backend timeout
- **WHEN** a backend does not answer in time
- **THEN** the client receives 504 TIMEOUT

### Requirement: Backend Credential Isolation
The gateway SHALL hold all backend credentials in its own secret store. It MUST NOT return them, or any backend address, to clients.

#### Scenario: Error from backend
- **WHEN** a backend returns an error
- **THEN** the client receives a mapped error code without backend hostnames or credentials

### Requirement: Request Size Limits
The gateway SHALL reject requests that exceed the mapped model's context window, or the configured maximum request size, with CONTEXT_LENGTH. It does this before forwarding the request to a backend.

#### Scenario: Oversized request
- **WHEN** a request's estimated tokens exceed the mapped model's context window
- **THEN** the gateway rejects it with CONTEXT_LENGTH without calling the backend

### Requirement: LLM Connections
The control plane SHALL store LLM connections, each with a unique name, an https base URL (http only for loopback in development), a credential, an enabled flag and a maximum number of concurrent requests. The credential MUST be encrypted at rest with a server key and MUST NOT be returned by any API; responses show only whether it is set and its last four characters. A disabled connection receives no requests. Every change is audited without the credential.

#### Scenario: Credential is write-only
- **WHEN** a platform-admin reads a connection after saving its credential
- **THEN** the response shows that a credential is set and its last four characters, and not the credential

#### Scenario: Disabled connection
- **WHEN** a connection is disabled while a role maps to one of its models
- **THEN** requests for that role fail with 503 MODEL_UNAVAILABLE, and no request reaches the disabled connection

### Requirement: Model Discovery
The gateway SHALL discover the models of each enabled connection from its OpenAI-compatible model list when the connection is saved, when an admin asks for it, and every 24 hours. Each discovered model keeps its upstream id and the time it was last seen. A model that is no longer listed stays known and is marked missing, so role mappings that use it are visible.

#### Scenario: Test a new connection
- **WHEN** a platform-admin saves a connection with a working base URL and credential
- **THEN** the gateway lists the connection's models and reports the connection as reachable

#### Scenario: Wrong credential
- **WHEN** the upstream refuses the credential during discovery
- **THEN** the connection is reported as failing with the upstream's status, and its earlier model list is kept

### Requirement: Model Capabilities From Connections
The capabilities the gateway reports in its Filtered Model List (kind, context window, maximum output tokens, tool, image and reasoning support, embedding dimension) SHALL come from the settings a platform-admin stores for each discovered model. A model without stored settings uses the gateway's defaults for its id, or chat with tools off when there is none.

#### Scenario: Admin sets a context window
- **WHEN** a platform-admin sets the context window of the model mapped to `main` and saves
- **THEN** the gateway's model list reports that context window for `main`

### Requirement: Role Mapping To Connection Models
A model id in policy.llm.models SHALL be resolved to a discovered model of an enabled connection. When several connections offer the same id, a mapping can name the connection as `<connection>/<model id>`; a bare id resolves to the first enabled connection that offers it, in connection order. A mapping that resolves to no known model fails with 503 MODEL_UNAVAILABLE.

#### Scenario: Two connections offer the same model
- **WHEN** connections `primary` and `backup` both offer `acme-llm-primary` and a Group maps `main` to `backup/acme-llm-primary`
- **THEN** that Group's requests for `main` go to `backup`
