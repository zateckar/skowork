# Spec Delta

## Purpose

Provides a local, deterministic stand-in for the subset of Microsoft Graph v1.0 that Skowork uses. Development, automated tests and demos can then exercise mail and calendar features without any real mailbox.

## ADDED Requirements

### Requirement: Graph Subset Compatibility
The mock SHALL implement every Graph v1.0 endpoint and query option that client/mail-calendar uses, with Graph-shaped requests, responses, paging links and errors. Coverage includes user profile, mailbox settings, mail folders, messages (list, get, filter, select, top, orderby, search, move, delete), attachments, sensitivity-label properties, message delta, drafts, send, events, calendar view, event delta, free/busy and event responses.

#### Scenario: Client runs against the mock
- **WHEN** the client is configured with the mock's base URL
- **THEN** every mail and calendar tool and the delta sync work without errors caused by unsupported endpoints or options

#### Scenario: New client endpoint
- **WHEN** a change makes client/mail-calendar call a Graph endpoint or property the mock does not cover
- **THEN** that change also extends the mock and its contract recordings before the client code is merged

#### Scenario: Unsupported endpoint
- **WHEN** a request targets an endpoint the mock does not implement
- **THEN** the mock responds with a Graph-shaped error stating that the endpoint is not mocked

### Requirement: Mock OAuth Endpoints
The mock SHALL provide OAuth 2.0 endpoints shaped like the Entra ones, so the client can run its real OAuth code path:
- discovery metadata;
- an authorize endpoint that auto-consents for configured test users and verifies PKCE;
- a token endpoint for the authorization_code and refresh_token grants, with configurable token lifetimes.

#### Scenario: Full sign-in against the mock
- **WHEN** the client runs the authorization code flow with PKCE against the mock for a test user
- **THEN** the mock returns an authorization code, then access and refresh tokens, and a later refresh_token grant returns a new access token

#### Scenario: Wrong PKCE verifier
- **WHEN** a token request presents a code verifier that does not match the code challenge
- **THEN** the token endpoint responds with an invalid_grant error

### Requirement: Mock OAuth Fault Injection
The mock SHALL let tests inject OAuth errors per test user or per request:
- interaction_required and consent_required from the authorize or token step;
- invalid_grant for a refresh token;
- expired access tokens.

#### Scenario: Interaction required
- **WHEN** a test injects interaction_required for the next silent token request
- **THEN** the token endpoint returns that error, and the client raises its "Connect mailbox" Question

### Requirement: Mock Authentication
The mock SHALL accept only bearer tokens issued by its own token endpoint, and its tokens MUST be useless anywhere else: they are signed with a key only the mock knows and name the mock as issuer. Requests without a valid mock token are rejected the way Graph rejects them.

#### Scenario: Missing token
- **WHEN** a request has no bearer token
- **THEN** the mock responds with an unauthorised Graph error

#### Scenario: Expired token
- **WHEN** a request presents a mock token whose lifetime has passed
- **THEN** the mock responds with an unauthorised Graph error, so the client exercises its refresh path

### Requirement: Deterministic Seeded Fixtures
The mock SHALL load mailbox and calendar fixtures from versioned fixture files, and produce identical data and identifiers for the same seed. The fixtures include:
- Czech, German and English content;
- threads with attachments;
- external and internal senders;
- sensitivity labels, private items and recurring events;
- several test users.

#### Scenario: Reproducible test data
- **WHEN** the mock is started twice with the same fixture set and seed
- **THEN** both instances return identical messages, events and identifiers

### Requirement: Captured Sending
The mock MUST never deliver mail to any real recipient. Sent messages and meeting invitations SHALL be captured, placed in the sender's Sent Items and, for internal test users, in the recipient's mock inbox.

#### Scenario: Agent sends a message
- **WHEN** the client sends a message through the mock to an external address
- **THEN** the message is captured and appears in Sent Items, and no network connection is made to any mail server

### Requirement: Fault Injection
The mock SHALL let tests inject faults per endpoint or per request: throttling with Retry-After, expired delta tokens, transient server errors, slow responses and malformed payloads.

#### Scenario: Injected throttling
- **WHEN** a test configures throttling on the messages endpoint for the next two requests
- **THEN** those two requests receive throttling responses with Retry-After, and the third succeeds

### Requirement: Test Inspection And Control API
The mock SHALL provide a control API, separate from the Graph surface, for tests to:
- reset state;
- load fixture sets;
- deliver new mail and create or change events, to drive delta sync;
- list captured sends;
- read the request log.

#### Scenario: Simulate incoming mail
- **WHEN** a test posts a new message to the control API for a test user
- **THEN** the next delta query for that user's inbox returns the message

### Requirement: Local And CI Operation
The mock SHALL run as a single self-contained process or container, with no external dependencies, and be usable both on a developer laptop and in CI pipelines.

#### Scenario: CI pipeline
- **WHEN** the CI pipeline starts the mock before the client integration tests
- **THEN** the mock is ready within 5 seconds and the tests run without internet access

### Requirement: Contract Alignment With Graph
The mock SHALL be checked by contract tests against recorded responses from a non-production Microsoft 365 test tenant. A response shape that deviates from the recordings fails the mock's test suite.

#### Scenario: Schema drift
- **WHEN** a mock response for a covered endpoint lacks a property present in the recorded Graph response
- **THEN** the contract test fails and names the endpoint and property
