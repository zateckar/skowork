# Spec Delta

## Purpose

Defines how an API connector is built from an uploaded OpenAPI document: which operations become tools, how they are described and limited, how they are authorized, and how the control plane calls the API on behalf of the user. The client never contacts the API.

## ADDED Requirements

### Requirement: API Document Upload
A platform-admin SHALL upload an API connector's definition as one file in JSON or YAML that declares OpenAPI 3.0.x or 3.1.x. The control plane MUST refuse any other format, including Swagger 2.0, a file larger than apiConnectors.maxDocumentBytes, and a document that references content outside itself. It MUST NOT fetch a definition from a URL.

#### Scenario: Swagger 2.0 refused
- **WHEN** a platform-admin uploads a Swagger 2.0 file
- **THEN** the upload is refused with a message that names OpenAPI 3.0 or 3.1 as the accepted versions

#### Scenario: External reference refused
- **WHEN** an uploaded document references a schema in another file or at another address
- **THEN** the upload is refused with a message that names the reference

### Requirement: Document Parsing Safety
The control plane SHALL parse an uploaded document with limits on nesting depth, alias count and expanded size. A document that exceeds a limit, or is malformed, MUST be refused with a validation error. Such a document MUST NOT crash the control plane or leave a partial connector stored.

#### Scenario: YAML alias expansion
- **WHEN** an uploaded YAML document expands its aliases beyond the limit
- **THEN** the upload is refused with a validation error and nothing is stored

### Requirement: API Description Rules
A correct description SHALL be text that, after trimming and collapsing whitespace, has 40 to 1,000 characters for an operation, and 80 to 2,000 characters for the API. It MUST NOT equal the API title, the path or the method and path, nor be a placeholder (TODO, TBD, string, description or n/a). The API MUST have a correct description, or the upload is refused.

#### Scenario: Short operation description
- **WHEN** an operation's description has 39 characters
- **THEN** the operation is listed as not usable, with the reason that its description is too short

#### Scenario: Copied title
- **WHEN** the API description equals the title of the API
- **THEN** the upload is refused with a message that the API needs its own description

### Requirement: Operation Usability
An operation SHALL be usable only if its method is get, put, post, patch or delete, it has a correct description, and its tool name is unique within the connector. The control plane SHALL list every operation of an upload with its status and, when it is not usable, the reason. An unusable operation MUST NOT be selected or offered.

#### Scenario: Placeholder description
- **WHEN** an operation's description is "TODO"
- **THEN** the report lists the operation as not usable with the reason that its description is a placeholder, and the other operations of the upload are kept

### Requirement: Parameter And Body Mapping
The control plane SHALL map an operation's path and query parameters to input properties of their own names, and a JSON request body to the property body. Header and cookie parameters MUST NOT be sent. An operation whose parameter name is not 1 to 64 characters of letters, digits, dots, underscores or hyphens, whose parameter is named body, or whose request body is not JSON, is not usable.

#### Scenario: Form request body
- **WHEN** an operation's request body is form data and not JSON
- **THEN** the operation is listed as not usable with the reason that only JSON request bodies are supported

### Requirement: Tool Schema Mapping
The control plane SHALL give each usable operation an input schema that is one JSON object, with its references resolved inside the document up to nesting depth 8. A reference that cannot be resolved, or that is deeper than 8, makes the operation not usable. The tool name SHALL be the operationId when it matches 1 to 64 characters of letters, digits, underscores and hyphens, and otherwise the method and path with other characters replaced by underscores.

#### Scenario: Reference too deep
- **WHEN** an operation's schema refers to itself through more than 8 levels
- **THEN** the operation is listed as not usable with the reason that the schema is too deep

### Requirement: Schema Budget
The control plane SHALL refuse a save that makes a connector select more than 100 tools, a tool schema larger than 16 KB, or a Group's connector schemas larger than 512 KB in total. The refusal happens when the selection or the assignment is saved, not when a bundle is built.

#### Scenario: Group over budget
- **WHEN** an administrator assigns a connector to a Group whose other connectors already use 500 KB of schemas, and the new connector adds 20 KB
- **THEN** the save is refused with a message that names the budget

### Requirement: Tool Selection
An administrator SHALL select which usable operations of a connector are tools, and no operation is selected by default. A save that selects an unusable operation MUST be refused. An operation that is new in a later upload is flagged and not offered until it is selected. An operation whose description or input schema changed is flagged the same way, and an operation that is no longer in the document is marked gone and its calls are refused.

#### Scenario: New operation flagged
- **WHEN** a re-uploaded document adds the operation "createOrder"
- **THEN** createOrder is flagged for review and is not offered to any client until an administrator selects it

### Requirement: Tool Risk Classes
Each selected operation SHALL have one risk class. The default is read for GET, external for POST, PUT and PATCH, and destructive for DELETE. An administrator MAY set another class. Setting a non-GET operation to read MUST be given a reason, and the audit trail MUST record the reason.

#### Scenario: Search by POST
- **WHEN** an administrator sets the POST operation "searchTickets" to read with the reason "search only"
- **THEN** clients receive risk class read for it, and the audit trail records the reason

### Requirement: Connector Authentication
An API connector SHALL use exactly one authentication mode: none, api-key, user-token or user-pat. The key or token of api-key and user-pat MUST be sent only in the request header the administrator names, and a key or token in the query string or the URL MUST be refused. The identity-provider token of user-token is sent only to a connector whose administrator chose that mode.

#### Scenario: Key in the query string
- **WHEN** an administrator names a query parameter as the place for the api-key
- **THEN** the save is refused with a message that the key must be sent in a header

#### Scenario: Identity-provider token only for chosen connectors
- **WHEN** an API connector uses none, api-key or user-pat
- **THEN** no request to its base URL carries the caller's identity-provider token

### Requirement: Personal Access Token Settings
For a connector in user-pat mode, a platform-admin SHALL configure the header that carries the token, an optional scheme, help text and an optional help link. The control plane MUST refuse the settings when a rule of the scenarios below is broken.

#### Scenario: Setting rules
- **WHEN** a platform-admin saves user-pat settings
- **THEN** the header name has 1 to 64 letters, digits or hyphens and is not a hop-by-hop or framing header (Connection, Host, Content-Length, Transfer-Encoding), the scheme is empty or 1 to 32 letters, the help text has 20 to 500 characters, and the help link, if any, is HTTPS

#### Scenario: Token settings saved
- **WHEN** a platform-admin sets the header to Authorization, the scheme to Bearer, and the help text to "Create a token under Profile, Personal access tokens"
- **THEN** the settings are stored, and a call to the connector sends "Authorization: Bearer <token>" with the caller's own token

#### Scenario: Missing help text refused
- **WHEN** a platform-admin saves a user-pat connector without help text
- **THEN** the save is refused with a message that users need help text to find where to create a token

### Requirement: Personal Access Token Storage
The control plane SHALL store each user's personal access token for each user-pat connector encrypted with the server key, and SHALL let only that user set, replace and remove it. The control plane MUST NOT return, log, audit, bundle or show a token to anyone else, and MUST send it only to the connector's registered base URL.

#### Scenario: Token stored encrypted
- **WHEN** a user sets a token for a user-pat connector
- **THEN** the stored value is not the token text, the status answer reports "set" with the time, and the token text appears in no response, log line or audit record

#### Scenario: Replacing a token
- **WHEN** a user sets a second token for the same connector
- **THEN** the second token replaces the first, and the next call uses the second token

#### Scenario: Another user's token
- **WHEN** a user asks for the token status of a connector
- **THEN** the answer covers only that user's token and never the token of another user

### Requirement: Personal Access Token Gating
A user-pat connector's selected operations SHALL be offered to a user only while that user has a token that is set and not rejected. Without such a token the control plane MUST NOT include the operations in the user's Policy bundle, and MUST refuse a call with a reason that names the missing token. The state of the connector for the user SHALL be one of ready, needs-token or token-rejected.

#### Scenario: Tools withheld without a token
- **WHEN** a user without a token for a user-pat connector receives the Policy bundle
- **THEN** the bundle carries the connector with the state needs-token and none of its operations

#### Scenario: Call without a token refused
- **WHEN** a user without a token calls an operation of the connector
- **THEN** the call is refused with the reason that a token is needed, and no request is sent

### Requirement: Personal Access Token Rejection
When the API answers a call made with a user's token with status 401, the control plane SHALL mark that token as rejected, SHALL withhold the connector's operations from that user, and SHALL record the rejection in the audit trail without the token. The user SHALL see the state token-rejected with the text "token rejected, replace it". Setting a new token clears the rejection. A 403 is not a rejection (see the scenario below).

#### Scenario: Rejected token withheld
- **WHEN** the API answers 401 to a call made with a user's token
- **THEN** the user's connector state becomes token-rejected, the next Policy bundle carries no operations for that user, and a later call is refused with the reason that the token was rejected

#### Scenario: Forbidden operation keeps the token
- **WHEN** the API answers 403 to a call made with a user's token
- **THEN** the call returns an error with the code FORBIDDEN, the upstream status and at most 2 KB of its text, the token state stays ready, and the operations stay in the bundle

#### Scenario: Replaced token restores tools
- **WHEN** the user replaces a rejected token with a new one
- **THEN** the rejection is cleared and the operations are offered again in the next Policy bundle

### Requirement: Personal Access Token Removal
The control plane SHALL delete every stored personal access token of a connector when an administrator removes the connector, and every token of a user when that user is revoked. The deletion SHALL be recorded in the audit trail with counts and without any token.

#### Scenario: Connector removed
- **WHEN** a platform-admin removes a user-pat connector that three users have tokens for
- **THEN** no token of that connector remains in storage, and the audit trail records the removal of three tokens

#### Scenario: User revoked
- **WHEN** a platform-admin revokes a user
- **THEN** that user's tokens for all connectors are deleted

### Requirement: Connector Listing For Users
The control plane SHALL list to each user the connectors of the user's Groups that the user may use, with each connector's kind, its state for that user (ready, needs-token or token-rejected), its number of offered tools, and for a user-pat connector its help text and help link. The listing MUST NOT contain any token or the registered base URL.

#### Scenario: Connector needing a token
- **WHEN** a user opens the connector listing and has no token for a user-pat connector
- **THEN** the listing shows that connector with the state needs-token, zero offered tools and its help text

### Requirement: Secret Handling
The control plane SHALL store an api-key and each personal access token encrypted with the server key of control-plane/llm-gateway. It SHALL accept an api-key write-only and SHALL replace it when an administrator enters a new value. It MUST NOT return a key or token through any interface, include it in a Policy bundle, or write it to a log or the audit trail.

#### Scenario: Key not shown
- **WHEN** an administrator opens an API connector that uses an api-key
- **THEN** the console shows that a key is set, and the stored value is not returned

### Requirement: Connector Call Authorization
The control plane SHALL execute an API call only for a user whose Groups have the connector enabled, for a selected and usable operation, with arguments that match its input schema, and, for a user-pat connector, only while the user's token is set and not rejected (Personal Access Token Gating). Any other call MUST be refused with a reason that the client can show, and nothing is sent to the API.

#### Scenario: Non-member call
- **WHEN** a user who is not in a Group of the connector calls one of its operations
- **THEN** the call is refused with the reason that the connector is not in the user's catalog, and no request is sent

#### Scenario: Invalid arguments
- **WHEN** a call omits a required path parameter
- **THEN** the call is refused before any request is sent, with the name of the missing parameter

### Requirement: Upstream Request Rules
The control plane SHALL send an API call only to the connector's base URL over HTTPS, or over HTTP to a loopback address when the development setting apiConnectors.allowLoopbackHttp is on, which is never the case in production. Path values MUST be percent-encoded, and the segments "." and ".." MUST be refused. The control plane MUST NOT follow redirects, MUST refuse link-local and cloud metadata addresses, and ends a call with no answer after apiConnectors.callTimeoutMs.

#### Scenario: Redirect answer
- **WHEN** the API answers a call with a redirect
- **THEN** the call returns an error that names the redirect status, and the redirect target is not requested

#### Scenario: Path traversal refused
- **WHEN** a path value is "..", so that the path would leave the base URL
- **THEN** the call is refused before any request is sent

### Requirement: Connector Concurrency
The control plane SHALL run at most apiConnectors.maxConcurrentCalls calls at once to one connector. A further call MUST be refused at once with a busy error that the client can show, and it MUST NOT wait in a queue.

#### Scenario: Ninth concurrent call
- **WHEN** eight calls to one connector are running and a ninth arrives
- **THEN** the ninth is refused with a busy error at once

### Requirement: Response Limits
The control plane SHALL return at most apiConnectors.maxResponseBytes of an API response, marked as truncated when it is cut, and SHALL return only JSON or text bodies. A response that is not 2xx MUST be returned as an error with its status and at most 2 KB of its text. The control plane MUST NOT store the response.

#### Scenario: Large response
- **WHEN** an API answers 300 KB of JSON to a call
- **THEN** the client receives the first 256 KB with a notice that the response was truncated

#### Scenario: Binary response
- **WHEN** an API answers an image to a call
- **THEN** the call returns an error that binary responses are not supported

### Requirement: Connector Call Audit
The control plane SHALL record each API call in the audit trail with the user, connector, operation, upstream status, duration, response size and the names of the arguments. The audit trail MUST NOT record argument values, the response body or any credential.

#### Scenario: Call recorded
- **WHEN** a user's call to the read operation "getOrder" succeeds
- **THEN** the audit trail holds one record with the user, the connector, "getOrder", status 200, the duration and the argument names, and no argument value

### Requirement: Egress For API Connectors
The control plane SHALL contact an API connector only at its registered base URL. The network policy of the client (policy.network) MUST NOT apply to these calls, and the registration takes its place.

#### Scenario: Other host in an argument
- **WHEN** a path value contains an absolute address of another host
- **THEN** the call is refused, and no request is sent to that host
