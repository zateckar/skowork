# Design

## Context

Facts from the repository on 2026-11-10:

- **Client UI says "Connections" for the MCP list.** The settings page id and title come from `settings.connections.title` (client/src/ui/strings/en.json:1059, used at client/src/shell/views_pages.cpp:33 and :647). The page lists MCP servers and the agent browser. Its strings are `settings.connections.*` (21 keys in en.json, lines 1059-1079 and later), plus `status_bar.mcp`, `status_bar.mcp_failed` and `status_bar.mcp_title` (en.json:84-86) and `apps.confirm_body` (en.json:795). Each key exists in de.json and cs.json as well, so about 75 translated strings change. client/build/debug/src/ui/bundles.cpp is generated from these files and is not edited.
- **Admin console says "LLM connections" for the LLM upstreams.** control-plane/admin/src/App.tsx:30 and views/connections.tsx:1. Its MCP view is labelled "MCP catalog" (App.tsx:34, :49, :62) and titled "MCP servers" (views/mcp.tsx:38, :73). The empty text and the URL placeholder also say MCP (mcp.tsx:45, :96).
- **The client's MCP code reads the bundle key `mcp.servers`.** client/src/mcp/manager.cpp:209 and tool ids are built as `mcp:<server>/<tool>` (manager.cpp:34).
- **Control-plane MCP code.** control-plane/src/mcp and its catalog spec (control-plane/mcp-catalog). Entries are assigned to Groups, versioned, audited and delivered in the Policy bundle.
- **No API connectors exist.** Nothing in client/ or control-plane/ reads OpenAPI.
- **In-flight base.** use-idp-tokens-for-mcp (7/8) replaces the audience token with the user's identity-provider token and changes Catalog Entry Rules and Entry Delivered In The Policy Bundle. This change modifies those requirements after that change, so the text it starts from is the text in use-idp-tokens-for-mcp/specs.
- **Secrets.** LLM connection credentials are encrypted with a server key derived from WORKBENCH_SECRET_KEY (control-plane/llm-gateway, Server Key From A Passphrase). API keys reuse that key.

## Goals / Non-Goals

**Goals**
- One word, Connectors, for everything the agent can reach outside the laptop.
- An OpenAPI document turns into tools whose descriptions are correct, checked by rule, before any operation is exposed.
- No API key and no API host is reached from the laptop.

**Non-Goals**
- Renaming internal names (policy key, tool ids, tables, capability names).
- A quality judgement of descriptions beyond the rules.
- Format support beyond OpenAPI 3.0.x and 3.1.x.

## Decisions

### D1. Naming: Connectors is the umbrella; kinds are mcp and api
- **Choice:** The user-facing term is Connector. A Connector has a kind: `mcp` (remote or local MCP server, as today) or `api` (an API connector). The policy key `mcp` and its `mcp.servers` list stay. An API connector is an entry of that list with `kind: "api"`. Tool ids are `mcp:<server>/<tool>` for MCP and `api:<connector>/<operation>` for APIs. The capability names `mcp-catalog` and `mcp-client` stay. The admin console keeps "LLM connections" and the client's status keys `status.connection.*` keep "Connection" because they describe the control-plane link.
- **Why:** The rename is visible to users and administrators. Internal names appear in skills (`requires_mcp`), Local apps (`mcp_tools`), policies (`policy.tools.rules`) and stored data. Changing them would migrate all of that for no user benefit.
- **Alternatives considered:**
  - Rename everything (policy key `connectors`, tool ids `connector:`, table names). This is a large migration with breaking changes to stored policies, skills and apps.
  - A separate API catalog with its own tables, policy key and admin view. This duplicates Group assignment, versioning and audit. The one-catalog rule of mcp-catalog stays.
  - Keep "MCP" in the UI and only add API. This contradicts the request.

### D2. Document format: OpenAPI 3.0.x and 3.1.x, uploaded as a file
- **Choice:** Accept JSON or YAML files that declare OpenAPI 3.0.x or 3.1.x. Refuse Swagger 2.0. Refuse external references. The control plane never fetches a document by URL.
- **Why:** Both 3.x versions cover the internal APIs the product owner named. Upload avoids the control plane fetching untrusted content and avoids a second network path.
- **Alternatives considered:**
  - Fetch by URL. This is a server-side request to an address the administrator types, and the document can change without a review.
  - Accept Swagger 2.0 through a converter. The converter adds a dependency and changes meaning in places. Left as open question 1.
  - Accept any version. Parsing and mapping rules differ between versions, so unknown versions are refused.

### D3. What "correct description" means
- **Choice:** Rules that a machine can check, with fixed numbers:
  - API description: 80 to 2,000 characters after trimming and collapsing whitespace. Required; a document without one is refused.
  - Operation description: 40 to 1,000 characters. Without one the operation is not usable.
  - Neither may equal the title, the path or the method and path, nor be a placeholder (`TODO`, `TBD`, `string`, `description`, `n/a`, case-insensitive).
  - Descriptions come from the definition only. An operation that fails is listed with its reason, and the rest of the upload is kept.
- **Why:** The model picks tools by description, so a short or copied description produces wrong calls. A machine rule gives the same answer every time. Refusing the whole API for one bad operation would block the rest of a large API.
- **Alternatives considered:**
  - Refuse the upload when any operation fails. Too strict for large definitions. Rejected.
  - Let the administrator override descriptions in the console. The definition would then not be the source of its own descriptions, and the audit would show a change that the document does not contain. Rejected; open question 2 asks whether the product owner wants it.
  - Ask a model whether a description is good. Costs a call per operation, is not repeatable, and would need the LLM API during upload. Rejected.
  - Accept any non-empty text. Lets through the copied summaries that the rules are meant to stop.

  The rules check form, not truth. A description that is well formed but wrong still passes. The administrator reviews the report before selecting operations (see Risks).

### D4. Operations become tools only by explicit selection
- **Choice:** Nothing is selected by default. The administrator selects usable operations, up to 100 per connector. A later upload flags new and changed operations (a changed description or schema), as Tool Discovery Snapshot does for MCP, and marks operations that disappeared as gone. A flagged operation is not offered until it is selected again.
- **Why:** Every exposed tool costs context in each Run, and a tool the model does not need makes it choose worse. Selection also keeps write operations out until an administrator has read them.
- **Alternatives considered:**
  - Expose all usable operations. Large APIs exceed the schema budget and dilute the tool list.
  - Select by tag or path prefix. Useful later; it is not needed to reach the first release.

### D5. Calls are executed by the control plane
- **Choice:** The client sends the arguments of an API tool call to the control plane. The control plane checks Group assignment, selection, usability and the input schema, builds the HTTP request, adds the credential, calls the API, limits the response and returns it. The client never contacts an API host.
- **Why:**
  - API keys stay on the server. Environment values of MCP entries must not hold secrets (mcp-catalog), and an API key in a Policy bundle would reach every laptop of the Group.
  - The workstation network block means unsigned builds cannot reach company hosts. The control plane is on-premise and reaches them.
  - Egress is controlled in one place. Each API connector is registered by an administrator, and the control plane contacts only that base URL.
  - The user's identity-provider token goes only to the control plane and then to the connector the user is allowed to call. It does not go to the laptop's network.
- **Consequences:**
  - Every API call takes one more hop through the control plane. The control plane also sees the response for a moment. It does not store it (see Response Limits in the control-plane spec).
  - API calls are an exception to client/policy-enforcement Network egress rules. policy.network does not apply to them, because the registration is the egress rule. This is stated in the control-plane spec (Egress For API Connectors).
- **Alternatives considered:**
  - Execute in the client, as MCP servers are. This needs the API keys in the bundle or a secret store on each laptop, needs egress rules per laptop, and fails under the workstation network block.
  - A separate connector service. A second process breaks the one-process rule for the control plane.

### D6. Authentication: none, api-key or user-token
- **Choice:**
  - `none`: no credential.
  - `api-key`: one value, stored encrypted with the server key, sent in a header the administrator names. Written only; never returned, logged or put in a bundle. A key in the query string is refused.
  - `user-token`: the caller's identity-provider access token as a bearer token, with the same rules as use-idp-tokens-for-mcp (no redirects). The administrator chooses this mode per connector (decided 2026-11-10).
  - `user-pat`: each user's own personal access token, sent in a header the administrator names with an optional scheme. The token is per user and connector (D11). A key or token in the query string is refused.
- **Why:** These cover the internal APIs described so far. Each mode uses an existing token, the existing key store, or the user's own token that the API issues to them.
- **Alternatives considered:**
  - OAuth client credentials per connector. It needs an identity-provider client per API. Out of scope (decided 2026-11-10; open question 3).
  - API key in the query string. It leaks into logs and proxies. Refused.
  - Mutual TLS. Needs certificate distribution to the server. Not in scope.

### D7. Tool names, input schemas and the derived name
- **Choice:**
  - The tool name is the operationId when it is present and matches `[A-Za-z0-9_-]{1,64}`. Otherwise it is the method and path, with each character outside that set replaced by `_`. Duplicate names make both operations unusable until the document is changed.
  - The input is one object. Path and query parameters are properties of their own names. A JSON request body is the property `body`. A parameter named `body` makes the operation unusable.
  - References are resolved inside the document up to depth 8.
  - Header and cookie parameters are not sent.
- **Why:** The name characters match the tool-name rules that model APIs accept, and a single object keeps the schema simple. Resolving references locally keeps the control plane from following external content.
- **Alternatives considered:**
  - Send the whole OpenAPI document to the model. Too large and includes unused operations.
  - One tool per method and path, with the path as an argument. Loses the operation's own schema.
  - Use the full mcp-style name with dots and slashes. Model APIs restrict function names; `[A-Za-z0-9_-]` is the safe set.

### D8. Risk classes: method defaults, override with reason
- **Choice:** The default risk is `read` for GET, `external` for POST, PUT and PATCH, and `destructive` for DELETE. An administrator may set another class. Lowering a non-GET operation to `read` requires a reason, which the audit records. Approvals follow policy.tools.rules and policy.tools.risk in client/policy-enforcement, and policy.unattended.allowed_risk decides unattended runs.
- **Why:** A POST can be a search, so a fixed rule would force approval on harmless calls; the reason and audit keep that choice visible. Defaults that are safe without any administrator work prevent a write from being `read` by accident.
- **Alternatives considered:**
  - Method only, no override. Blocks POST search endpoints from being used without approval for every call.
  - No default, the administrator must set every class. Forgotten operations would default to the least safe class.

### D9. Limits
- **Choice (server settings, not policy keys):**
  - Document: 5 MB (`apiConnectors.maxDocumentBytes`).
  - Operations: 300 per document. Selected tools: 100 per connector.
  - Tool schema: 16 KB. Connector schemas per Group: 512 KB in total.
  - Call arguments: 64 KB. Response: 256 KB, then truncated. Error text: 2 KB.
  - Call timeout: 30 s. Concurrent calls per connector: 8.
- **Why:** The numbers keep one connector from filling the bundle or a Run's context, and keep the control plane's memory bounded during a call.
- **Alternatives considered:**
  - Policy keys for these limits. They are server protection, not user policy; the glossary keeps those out of the policy.

### D10. Address rules and the development fake API
- **Choice:** The base URL is HTTPS. HTTP is accepted only for a loopback address and only when the control plane runs in development mode, so the local fake API (dev/api-mock) can be reached. Link-local and cloud metadata addresses are always refused. Private addresses are allowed, because internal APIs are the purpose. Redirects are not followed.
- **Why:** Internal APIs usually sit on private addresses, so blocking them would stop the feature. Refusing link-local addresses removes the usual server-side request forgery target. Development needs a local API without certificates.
- **Alternatives considered:**
  - Allow only public addresses. Rejected for the reason above.
  - Block all private ranges and allow only a host list. Host lists are needed later for connector hosts, but they do not replace the registration.
- **Setting:** HTTP to a loopback address is allowed only when the server setting apiConnectors.allowLoopbackHttp (WORKBENCH_API_ALLOW_LOOPBACK_HTTP) is on. The setting is forced off in production. It stands for the development mode named above, so that tests can use a local fake API.

### D11. Personal access tokens: per user and connector, held by the control plane
- **Choice (decided 2026-11-10):**
  - A platform-admin marks an API connector as `user-pat` and sets the header, the optional scheme, the help text and the optional HTTPS help link (control-plane/api-connectors Personal Access Token Settings).
  - Each user sets, replaces and removes their own token for each such connector through the user API (see API contract below). The control plane stores it in table `api_connector_tokens`, keyed by connector and user, encrypted with the server key in the same format as the api-key (D6).
  - The status answer says only whether a token is set and when it was set. The token is never returned, logged, put in the audit trail, put in a bundle, or shown to another user.
  - The control plane sends the token only to the connector's registered base URL, in the configured header, with the configured scheme before it.
  - An upstream 401 to a call made with a user's token marks that token rejected. The user sees "token rejected, replace it", and the connector's operations are withheld from that user until a new token is set. An upstream 403 is a permission refusal for that operation: it is returned to the agent as FORBIDDEN with the upstream status and sanitized text, and the token state does not change. (Changed 2026-11-10 from "401 or 403": a per-operation 403, as Jira answers per project and operation, would otherwise withhold every tool of the connector.)
  - Deleting a connector deletes all its tokens. Revoking a user deletes that user's tokens.
  - The user's bundle carries the operations only while the token is usable. Setting, rejecting, replacing or removing a token rebuilds that user's bundle.
- **API contract for the client (control plane, all under requireUser, the caller only):**
  - `GET /api/v1/connectors` returns `{ connectors: [{ id, name, kind: "mcp" | "api", auth: "none" | "api-key" | "user-token" | "user-pat", state: "ready" | "needs-token" | "token-rejected" | "not-applicable", offered_tools: number, help_text?: string, help_link?: string, token?: { set: true, set_at: string } }] }`. It lists only connectors of the user's Groups. It never contains a token, a base URL or a key. `state` is `not-applicable` for kinds and modes that need no token. `token` is present only for user-pat connectors with a token.
  - `PUT /api/v1/connectors/:id/token` with body `{ "token": "<text>" }` sets or replaces the caller's token. It answers `{ id, state, token: { set: true, set_at } }` and never echoes the token. It answers 404 for a connector outside the caller's Groups and 400 for an empty token, a token longer than 4,096 characters, or a connector not in user-pat mode.
  - `DELETE /api/v1/connectors/:id/token` removes the caller's token. It answers `{ id, state, token: null }`.
  - Tool listing and execution use the bundle and the existing call path: the client offers the operations in `policy.mcp.servers` (kind `api`) and sends a call to `POST /api/v1/connectors/:id/operations/:operation/call` with `{ "arguments": {...} }`. The answer is `{ status, body, truncated }` for a 2xx response, or the error `{ error: { code, message, field?, reason?, status?, text? } }`. Codes: NOT_OFFERED, DENIED and FORBIDDEN (403; FORBIDDEN is an upstream 403 with its status and text), TOKEN_MISSING and TOKEN_REJECTED (403), VALIDATION_FAILED (400, with `field`), TOO_LARGE (413), UNSUPPORTED_CONTENT (415, binary), BUSY (429), UPSTREAM_ERROR (502, with the upstream `status` and at most 2 KB of `text`), TIMEOUT (504). The bundle entry of kind api is `{ id, name, kind: "api", auth, state, operations: [{ name, description, risk, input_schema }], include_tools: [], exclude_tools: [], risk: { <name>: <risk> } }`; a client offers `api:<id>/<name>`. (These paths are the contract; the spec names the behaviour.)
- **Why:**
  - The API issues a token to each user and needs it to tell users apart. A shared key cannot do that (api-key stays for shared keys).
  - D5 says only the control plane contacts API hosts, so only the control plane can send the token. The laptop is under the workstation network block and cannot reach company hosts.
  - Encryption at rest with the server key keeps the token off laptops. A token in a bundle would reach every laptop in the Group.
  - Only a 401 marks the token rejected. A 403 is a refusal of one operation and is returned as FORBIDDEN, so one forbidden project or operation does not withhold the connector's tools (changed 2026-11-10).
- **Alternatives considered:**
  - Keep the token on the laptop and let the client send it. Breaks D5 and the network block, and the token would need to be in the client on every call.
  - One shared api-key per connector entered by the administrator. Loses the per-user identity at the API and in its own audit. Covered by api-key for APIs that accept a shared key.
  - Use the user's identity-provider token (user-token). Most APIs do not accept an identity-provider token in place of their own token. user-token stays for APIs that do.
  - Put the token in the Policy bundle. Every laptop of the Group would hold it. Rejected.
  - Store it in plain text, or in a new secret store. Plain text is rejected. A new secret store adds a dependency; the server key already protects the other credentials.
  - Treat 401 and 403 both as rejected (the first decision of 2026-11-10). Rejected: one forbidden operation would withhold every tool of the connector.

## Reversed and extended decisions

No earlier decision is reversed. Four are extended or excepted, and they are named here so that they are read together.

- **use-idp-tokens-for-mcp, D1 and the Remote Server User Token rule.** The user's token goes to remote MCP servers from the client. For API connectors the token goes from the control plane, and only to the registered base URL of a connector whose mode is user-token. The rule that tokens never go to an address an entry did not name stays.
- **D6 and D11, personal access tokens.** A fourth mode, user-pat, is added. Its tokens are the user's own, held by the control plane. They follow the same address rule as user-token: only the registered base URL receives them.
- **add-core-tools-and-mcp, D8 (the client runs the MCP manager and the connections to servers).** Unchanged for MCP. API calls are executed by the control plane (D5). This is an exception for API connectors only.
- **client/policy-enforcement, Network egress rules.** Calls to an API connector are not subject to policy.network. The registration and the control plane's address rules govern them (D5, D10).
- **control-plane/mcp-catalog, Catalog Entry Rules and Entry Delivered In The Policy Bundle.** Modified to add the kind `api`. The rule that an environment holds no secrets is kept, and API keys are stored as secrets outside the bundle.
- **control-plane/mcp-catalog, Purpose.** The purpose statement still says "MCP servers". Spec purpose text is not part of a delta, so it is updated when this change is archived.

## Glossary changes

Owned by the product owner. These changes are needed in openspec/config.yaml. They are not made here.

- **Connector** (new): an external system the agent may use, approved in the catalog and assigned to Groups. Its kinds are `mcp` (an MCP server) and `api` (an API connector). The UI calls both Connectors. "MCP server" stays the term for kind `mcp`.
- **API connector** (new): a Connector whose operations come from an uploaded OpenAPI document.
- **Operation** (new): one HTTP method and path of an API connector's document. A selected operation is a Tool.
- **Tool**: add `api:<connector>/<operation>` to the list of tool ids.
- **Policy key layout**: `policy.mcp.servers` lists Connectors of kinds `mcp` and `api`. The key `mcp` stays.
- **Connections**: the UI word for the Connectors page is retired. "LLM connections" and "Connection" (control-plane link) stay.

## Risks / Trade-offs

- [Risk] A description passes the rules but is wrong, so the model calls the operation for the wrong purpose. -> Mitigation: the report is shown before selection, selection is explicit (D4), and writes need approval by risk class (D8).
- [Risk] A description is written as an instruction, for example "always call this first", and the model follows it. Descriptions are model-visible text. -> Mitigation: the Connectors page and the tool description label the text as set by the administrator; descriptions never change a permission or a risk class, which come only from the catalog and policy.
- [Risk] The control plane becomes a data path for every API call, and a slow API ties up its memory. -> Mitigation: 30 s timeout, 8 concurrent calls per connector, 256 KB response cap, no storage of responses (D9).
- [Risk] Server-side request forgery to an internal host an administrator did not intend. -> Mitigation: only administrators register connectors, link-local addresses are refused, redirects are not followed, the base URL is fixed in the registration (D10).
- [Risk] An API key leaks through the audit, a log or a bundle. -> Mitigation: write-only field, encrypted storage, the audit records argument names only, and a test checks that a key does not appear in the bundle, the audit or the logs.
- [Risk] A YAML document expands into a very large structure (alias bombs, deep nesting). -> Mitigation: parser limits on depth, aliases and expansion size, and a fuzz test (control-plane/api-connectors Document Parsing Safety).
- [Risk] A POST operation that only reads is classed `external` and needs an approval each time. -> Mitigation: the administrator lowers it with a reason (D8); the approval prompt shows the operation's description.
- [Risk] The user's identity-provider token reaches an API host the user did not choose. -> Mitigation: only an administrator registers the host, and only user-token connectors receive the token. This is the same trust decision as use-idp-tokens-for-mcp, and the open question below asks whether it is acceptable for APIs.
- [Risk] The document changes upstream and an operation disappears or changes meaning. -> Mitigation: removed operations are marked gone and their calls are refused; a changed operation is re-reviewed when the new document is uploaded.

- [Risk] A user's token is sent with a call to a host it was not meant for, or is written to a log. -> Mitigation: the token goes only to the registered base URL (D11), the audit and logs hold no token, and tests check both.
- [Risk] A 401 caused by a wrong or expired token is not told apart from a revoked one. -> Mitigation: both show the state token-rejected, and replacing the token restores the tools (D11). A 403 never changes the token state (changed 2026-11-10).
- [Risk] A user keeps an old token that is valid but was issued for someone else's account. -> Mitigation: a token is set only by its own user through the user API, and only that user's bundle and status show it.

## Open questions for the product owner

Answered on 2026-11-10 (recorded above):

1. Swagger 2.0 is out of scope (Proposal, D2).
2. Administrators cannot override a description in the console (D3).
3. OAuth client-credentials are out of scope (D6).
4. Yes, but only when an administrator picks the user-token mode for a connector (D6, Risks).

Open:

5. Should the agent browser row stay on the Connectors page, or move under a separate "On this computer" heading?

## Acceptance

Control plane and admin console, recorded on 2026-11-10 (tasks 2 to 5 and 7.3 are checked; the rest are open):

- `openspec validate --all --strict`: 49 of 49 pass, with no warnings for this change.
- `npm --prefix control-plane run check`: passes.
- `npm --prefix control-plane run admin:build`: passes.
- `npm --prefix control-plane test`: 304 of 304 pass. The baseline before this change was 276. The 28 new tests are in `test/api-connectors.test.ts`, which runs against a local fake API and never an external host. Round 2 added the 403 (FORBIDDEN, token stays ready) and user-token path tests. `test/mcp.test.ts` (migration test) was extended by the new migration's tables.
- Not run, and why: 1.1 and 1.2 (the product owner's documents and the organization LLM API are needed), 6.1 to 6.4 (client, built by another agent), 7.1 and the browser part of 7.2 (browser checks), 8.1 to 8.3 (end to end and desktop), 9.1 (archive waits for use-idp-tokens-for-mcp and every task).
