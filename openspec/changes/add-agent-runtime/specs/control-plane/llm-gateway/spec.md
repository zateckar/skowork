# Spec Delta

## Purpose

Defines the central gateway that is the only path from clients to the organization LLM API. It authenticates users, maps model roles to upstream models per Group, enforces budgets and rate limits, audits usage, and fails over between backends.

## ADDED Requirements

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
The gateway SHALL forward requests to the organization LLM API, which is an OpenAI-compatible endpoint. The gateway authenticates to it with a credential held only by the gateway. The upstream address and credential are gateway server settings.

#### Scenario: Upstream credential never reaches clients
- **WHEN** a client calls any gateway endpoint
- **THEN** the gateway adds its own upstream credential, and neither the credential nor the upstream address appears in any response to the client

#### Scenario: Development upstream
- **WHEN** the gateway runs in its development configuration
- **THEN** it reads the upstream address and credential from the environment variables OPENAI_BASE_URL and OPENAI_KEY, refuses to start if either is missing, and has no fallback to a mock backend

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
