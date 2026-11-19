# Spec Delta

## MODIFIED Requirements

### Requirement: Organization LLM API Upstream
The gateway SHALL forward requests to the LLM connections that platform-admins manage (LLM Connections). Each connection is an OpenAI-compatible endpoint, normally the organization LLM API. The gateway authenticates to it with the connection's credential, which is held only by the gateway.

#### Scenario: Upstream credential never reaches clients
- **WHEN** a client calls any gateway endpoint
- **THEN** the gateway adds the connection's credential, and neither the credential nor the upstream address appears in any response to the client

#### Scenario: Development upstream
- **WHEN** the control plane starts in its development configuration with no LLM connection stored and the environment variables OPENAI_BASE_URL and OPENAI_KEY set
- **THEN** it creates a connection named `primary` from them, and there is no fallback to a mock backend

## ADDED Requirements

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
