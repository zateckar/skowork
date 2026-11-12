# Tasks

Test commands: control plane `npm --prefix control-plane test`; client `cmake --build --preset test` in client/. Tasks marked **[needs user OK: desktop-input]** drive the desktop client and need the user's permission first (sign-in each time). Tasks that use the real organization LLM API run locally only, and the key is never committed.

## 1. Validation spike

- [ ] 1.1 Obtain three real internal OpenAPI documents from the product owner (one 3.0, one 3.1 JSON, one 3.1 YAML). Run the D3 description rules and the D7 mapping over them by hand or with a throwaway script in dev/scripts. Verify: a table of counts per document (operations usable, unusable with reason) is recorded in design.md, Context.
- [ ] 1.2 Check that the organization LLM API accepts 100 tools with 16 KB schemas and names matching `[A-Za-z0-9_-]{1,64}` (dev/scripts, real API, local only). Measure the first-token time on the baseline hardware with the bundle at its maximum size. Verify: the result, including the accepted names and the time, is recorded in design.md, Context. If the time exceeds the first-token limit of control-plane/llm-gateway, change D9 before continuing.

## 2. Catalog kind and rename

- [x] 2.1 Catalog entries of kind `api`: base URL, authentication mode (none, api-key, user-token), id rule, and the refusal of query-string keys and HTTP addresses (except loopback in development). Verify: unit tests for invalid id, HTTP base URL in production mode, query-string key, api-key without a value, and an existing `mcp` entry that loads unchanged.
- [x] 2.2 Group assignment, versioning and audit for `api` entries, with the same rules as MCP. Verify: unit tests that two Groups give the union, that each change records a version and an audit row with the entry before and after, and that the audit row holds no key value.
- [x] 2.3 API Connector Bundle Entry: an `api` entry is embedded in policy.mcp.servers with its selected operations, and carries no base URL, secret or document. Verify: bundle test for a developer and a non-member; a disabled entry is absent from the next bundle; a save that would push a Group above 512 KB of schemas is refused.

## 3. OpenAPI documents

- [x] 3.1 Document parsing with the D9 and Document Parsing Safety limits; JSON and YAML; 3.0.x and 3.1.x; Swagger 2.0 and external references refused. Verify: unit tests for each refusal, and a fuzz test over mutated and random inputs that asserts no crash and no partial entry.
- [x] 3.2 Description rules (D3): API and operation length bounds, title and path equality, placeholders. Verify: table-driven unit tests at 39, 40, 1,000 and 1,001 characters for an operation, and at 79, 80, 2,000 and 2,001 for the API, plus a placeholder and a copied title.
- [x] 3.3 Operation usability and naming (D7): methods, derived names, duplicates, parameter names, header parameters ignored, non-JSON bodies, a parameter named `body`. Verify: unit tests, each reason shown in the report.
- [x] 3.4 Tool schema mapping: path and query parameters, `body`, local references up to depth 8, a reference cycle, and the 16 KB limit. Verify: unit tests with golden schemas, and a test that a reference beyond depth 8 makes the operation unusable.
- [x] 3.5 Upload route, report and selection: save refuses an unusable operation; a later upload flags new operations and marks removed ones gone; 100 selected tools per connector at most. Verify: API tests against the control plane's HTTP routes, one per rule.

## 4. Risk, authentication and secrets

- [x] 4.1 Risk defaults by method and the override with a reason (D8). Verify: unit tests for each default; a non-GET set to `read` without a reason is refused; with a reason the audit row holds it.
- [x] 4.2 Authentication modes and secret storage (D6, Secret Handling). Verify: tests that the key is stored encrypted, that no API response, bundle or log line contains it, and that entering a new value replaces the old one.
- [x] 4.3 Personal access token settings for user-pat connectors (Personal Access Token Settings, D11): header and scheme rules, hop-by-hop headers refused, help text 20 to 500 characters required, help link HTTPS only. Verify: table-driven unit tests for each refusal and for a valid save; a user-pat connector saved without help text is refused.
- [x] 4.4 Personal access token storage and user API (Personal Access Token Storage, Personal Access Token Removal, D11): `PUT` and `DELETE /api/v1/connectors/:id/token`, status in `GET /api/v1/connectors`, encryption at rest, deletion on connector removal and user revocation. Verify: tests that the table holds no token text, that no response, bundle, log line or audit row contains the token, that a second user's status does not show the first user's token, that another user's connector is answered 404, and that removal deletes the tokens and records their count.
- [x] 4.5 Token gating and rejection (Personal Access Token Gating, Personal Access Token Rejection): the bundle omits operations for users without a usable token, a call without a token is refused, an upstream 401 marks the token rejected and the next bundle omits the operations, replacing the token restores them, and an upstream 403 returns FORBIDDEN and keeps the token ready (changed 2026-11-10). Verify: tests against the local fake API: a 401 answer marks the token rejected, the bundle of that user changes, a second user's bundle is unchanged, the rejection audit row holds no token, and a 403 leaves the token state ready.
- [x] 4.6 Connector listing for users (Connector Listing For Users): `GET /api/v1/connectors` for the caller's Groups with kind, auth, state, offered tools and help text, and no token or base URL. Verify: test that a non-member's connector is absent, and that the response text contains neither the base URL nor a token.

## 5. Call execution

- [x] 5.1 Call authorization and argument validation (Connector Call Authorization). Verify: tests against a local fake API in the test process: a non-member is refused, an unselected operation is refused, invalid arguments are refused before any upstream request is made.
- [x] 5.2 Upstream request rules: HTTPS, loopback HTTP only in development mode, path encoding, `.` and `..` segments refused, link-local refused, no redirects, 30 s timeout, 8 concurrent calls. Verify: tests against the fake API that check the exact request received (path, query, headers, body), a redirect answer that yields an error, and a ninth concurrent call that is refused.
- [x] 5.3 Response limits and errors: 256 KB truncation with a notice, JSON and text only, binary refused, non-2xx returned with status and 2 KB of text, nothing stored. Verify: tests with a 300 KB response, a 500 response, a binary response, and a check that the database holds no response body.
- [x] 5.4 Connector call audit (Connector Call Audit). Verify: a test that the audit row holds user, connector, operation, status, duration, size and argument names, and holds no argument value, body or key.
- [x] 5.5 Egress for API connectors: the control plane contacts only the registered base URL, and the network policy is not applied to these calls. Verify: a test that a path value holding an absolute address of another host is refused, and that no request reaches that host.

## 6. Client

- [x] 6.1 API Connector Tools and Connector Calls Through Control Plane: the client offers the selected operations as `api:<connector>/<operation>`, validates arguments locally, and sends the call to the control plane. Verify: client tests (cmake --build --preset test) with a fake control plane over local HTTP: the tool list matches the bundle, an invalid argument sends nothing, and no request reaches the API host.
- [x] 6.2 Risk and approval wiring and untrusted results (Risk And Approval, API Results Are Untrusted): a write operation with policy approve asks, a denied call is not sent, and a result has provenance `api:<connector>/<operation>`. Verify: client tests for each decision and the provenance value.
- [x] 6.3 Connectors page and strings: rename `settings.connections.*`, the status bar keys and `apps.confirm_body` in en.json, de.json and cs.json; show API connectors with state and usable tool count. Verify: a test that every key exists in all three files and that the English text for the old keys is gone; client tests for the page model.
- [x] 6.4 Personal token entry (Personal Token Entry): the Connectors page shows needs-token and token-rejected states with "Add token" and the text "token rejected, replace it"; the masked field sends the token once to `PUT /api/v1/connectors/:id/token`, is cleared afterwards, and a remove action sends `DELETE`. Verify: client tests with a fake control plane over local HTTP: the request body and path, the cleared field, no token in settings, logs or the bundle cache, and the state text for each state.

## 7. Admin console

- [ ] 7.1 Connectors view (code in place; browser check not run) (Connector Administration): list both kinds, upload dialog for an OpenAPI file, the per-operation report with reasons, and the selection, risk, authentication and write-only key fields (Connector Selection And Risk). Verify: a browser check against the local control plane with dev/api-mock: an upload with one bad operation shows its reason, and the save of the bad operation is refused.
- [x] 7.2 Rename the admin labels: navigation "Connectors", the view title and description, and the MCP view's titles and text in App.tsx and views/mcp.tsx. Verify: a test or a search that finds no "MCP catalog" label in control-plane/admin/src, and a browser check of the navigation group. (Browser check of the navigation group not run; the search and admin build pass.)
- [x] 7.3 Personal access token settings in the connector form (Personal Access Token Settings In Console): for user-pat, header, scheme, help text and help link fields, the control plane's refusal shown next to the field, and the count of users with a token. Verify: control-plane API tests for the fields' save and refusal; `npm --prefix control-plane run admin:build` passes; the visual check is part of 7.1.

## 8. End-to-end and acceptance

- [ ] 8.1 Create dev/api-mock (done: mock created and smoke-tested; the Run is not run): a local fake API with a 3.1 OpenAPI document, one read and one write operation, and one operation with a bad description. Run a Run against the local stack with the real organization LLM API: the agent calls the read operation, and the write operation asks for approval. Verify: the audit rows of 5.4 exist for both calls, and the transcript shows the untrusted provenance. Local only.
- [ ] 8.2 **[needs user OK: desktop-input]** In the desktop client, open Connectors in Settings, and approve one write call. Verify: the page shows the API connector and its tool count, and the approval dialog shows the operation's description. Run once; the user signs in once.
- [ ] 8.3 Run `npm --prefix control-plane test`, `cmake --build --preset test` in client/, and `openspec validate --all --strict`. Verify: all three pass; record the results in design.md, Acceptance.

## 9. Archive

- [ ] 9.1 After use-idp-tokens-for-mcp is archived and all tasks above are checked, run `openspec archive add-api-connectors --yes`, then reread control-plane/mcp-catalog, client/mcp-client and control-plane/admin-console for coherence, and update the mcp-catalog Purpose text. Verify: `openspec validate --all --strict` passes after archiving.
