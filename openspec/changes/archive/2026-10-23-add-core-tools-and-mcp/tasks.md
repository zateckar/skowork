# Tasks

## 1. File tools (client/core-tools)

- [x] 1.1 Wire every file tool to the policy-enforcement path check and handle-based access. Verify that a junction-escape fixture is denied without reading data
- [x] 1.2 Implement files.read, files.list, files.glob and files.grep with deny filtering, binary refusal (except supported documents) and truncation. Verify with unit tests for each requirement scenario
- [x] 1.3 Implement files.write and files.edit with atomic replace and exact-once matching. Verify the ambiguous-match and outside-permitted-folder tests
- [x] 1.4 Implement the extract worker role with the docx, xlsx, pptx and PDF extractors and limits. Verify with a document corpus test, the malformed-document scenario, and fuzz targets for each format

## 2. Shell, web fetch and downloads (client/core-tools)

- [x] 2.1 Implement shell.run (powershell 5.1 default, pwsh optional, unavailable-shell handling) through the tool-sandbox process API, passing the per-call timeout argument (max 30 minutes) to the sandbox, which applies the min-wins rule. Include process-tree kill and complete results to the runtime. Verify the timeout, policy-limit and mode-off tests
- [x] 2.2 Implement the destructive-pattern classifier driven by policy.tools.shell.destructive_patterns. Verify with a test corpus of destructive and benign commands
- [x] 2.3 Implement web.fetch with the egress check per request and redirect, integrated authentication limited to listed hosts, HTML and document text extraction, and provenance marking. Verify with tests against a local test server, and fuzz the HTML extractor
- [x] 2.4 Implement the quarantine downloads folder with mark-of-the-web. Verify that the Zone.Identifier stream is set and that execution is denied without approval

## 3. MCP client (client/mcp-client)

- [x] 3.1 Implement the JSON-RPC/MCP protocol core and the stdio transport. Verify against the MCP reference test server and with protocol fuzz targets
- [x] 3.2 Implement the Streamable HTTP transport with audience-scoped token acquisition. Verify that the token audience equals the server in an integration test
- [x] 3.3 Implement pinned local package fetch, hash check and sandboxed start. Verify that a tampered package is refused
- [x] 3.4 Implement namespacing, include/exclude filtering, catalog risk classes and untrusted result marking. Verify that the excluded-tool and server-claims-read-only scenarios pass
- [x] 3.5 Implement the lifecycle (lazy start, idle stop, restart limit), health reporting in Connections and the status bar, and live catalog updates. Verify the crash-loop and no-usable-tools scenarios

## 4. MCP catalog (control-plane/mcp-catalog)

- [x] 4.1 Implement the catalog entries, versions, Group assignment and audit in SQLite with an admin API. Verify with API tests (`npm test`, node:test)
- [x] 4.2 Implement the tool discovery snapshot with review-before-enable. Verify that a new remote tool stays excluded until reviewed
- [x] 4.3 Implement the content-addressed package endpoint. Verify that downloads require a token and that the bytes match the hash
- [x] 4.4 Notify control-plane/policy-service of affected Groups on every catalog change, so it rebuilds bundles and sends policy-changed. Verify that a catalog edit reaches a connected test client
- [x] 4.5 Implement the Audience-Scoped Token Exchange and exchanged-token signing key publication in control-plane/identity (moved from add-client-foundation), with the catalog as the audience source. Verify that a token for an assigned server carries that server as its only audience, that an unassigned audience is refused and audited, that a token from a client that is not allowed is refused, and that a key rotation keeps unexpired tokens valid

## 5. Integration

- [x] 5.1 End-to-end test: a developer-Group user asks the agent to search Jira through the catalog server, edit a local file and run a build command in the sandbox. Verify that every call passes the policy hook and that its provenance appears in the Thread
- [x] 5.2 Run `openspec validate --all --strict` and fix any findings

## Workflow follow-up

- Archive this change with `openspec archive add-core-tools-and-mcp --yes` after review, then check that the merged specs read coherently.
