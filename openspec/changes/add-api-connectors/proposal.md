# Proposal

## Why

On 2026-11-10 the product owner asked for two things:

- **Connectors.** The organization's external systems should be called "Connectors" instead of "MCP". A Connector is either an MCP server or an API. Today every user-facing place says "MCP" or "Connections", and the admin console already uses "connections" for LLM connections.
- **API connectors.** Employees need the agent to call internal REST APIs. Today only MCP servers can be used. An API has no tool list of its own, so the organization needs an OpenAPI definition for each API. Every API and every operation exposed to the agent needs a correct description, because the model chooses tools by their descriptions.

## What Changes

- **Renaming (user-facing only).** "MCP" becomes "Connectors" in the client settings page, the status bar, the Local apps confirmation text and the admin console navigation and titles. The internal `mcp` policy key, the `mcp:<server>/<tool>` tool ids, the database tables and the capability names `mcp-catalog` and `mcp-client` are kept. The admin console keeps "LLM connections" for LLM upstreams.
- **Kinds.** The catalog gets a parallel kind, `api`, next to the MCP kinds remote and local. An API connector is assigned to Groups, versioned and audited like any entry.
- **Upload.** A platform-admin uploads an OpenAPI 3.0.x or 3.1.x document (JSON or YAML). The control plane checks every description against fixed rules and shows a report per operation.
- **Descriptions.** The API needs a correct description. An operation needs one too. An operation without a correct description is not usable, cannot be exposed, and is listed with its reason. The upload is refused only when the API itself has no correct description or the document cannot be read.
- **Tools.** A platform-admin selects which usable operations become tools. Each selected operation becomes the tool `api:<connector>/<operation>`. It takes its risk class from its method by default, and the policy decides approvals as for every tool.
- **Calls.** The control plane makes the HTTP call to the API. The client sends the arguments to the control plane and never contacts the API host. The control plane holds API keys and checks each call against Group assignment, selection and the input schema.
- **Personal access tokens.** An API connector can require each user's own personal access token, for example for Jira. A platform-admin configures the header, the scheme and the help text. Each user adds, replaces and removes their own token on the Connectors page. The control plane stores it encrypted, never returns it, and sends it only to the connector's base URL. Until a user has a usable token, the connector's tools are not offered to that user.
- **Limits.** Schemas, selected tools, responses and call time have fixed limits. Responses are untrusted content.

Out of scope:
- Swagger 2.0 and other API description formats (decided 2026-11-10: out of scope).
- Uploading by URL. The control plane never fetches a definition.
- OAuth client-credentials and other authentication schemes for API connectors. Only none, api-key, user-token and user-pat are in scope.
- Overriding a description in the console. Descriptions come from the definition only (decided 2026-11-10: no overrides).
- Automatic description quality checks by a model, and automatic selection of operations.- Renaming the policy key `mcp`, the `mcp:` tool ids, the database tables or the capability names. This is a later change, if ever.
- Streaming and binary responses, and API request bodies other than JSON.
- Connectors that the user adds on their own machine.

## Capabilities

### New Capabilities
- `control-plane/api-connectors`: OpenAPI upload and parsing, description rules, operation usability, tool selection, schema mapping and limits, risk defaults, authentication and secrets, call execution, and call audit for API connectors.
- `client/api-connectors`: offering the selected operations to the agent, sending calls to the control plane, risk and approval wiring, untrusted results and the status of API connectors.

### Modified Capabilities
- `control-plane/mcp-catalog`: Catalog Entries, Catalog Entry Rules and Entry Delivered In The Policy Bundle are modified for the `api` kind. API Connector Bundle Entry is added.
- `client/mcp-client`: Health And Diagnostics is modified so that the settings page is called Connectors.
- `control-plane/admin-console`: Grouped Navigation is modified for the new label. Connector Administration and Connector Selection And Risk are added.

## Impact

- **Roadmap:** 43 [Managed], in the same category as add-llm-connections. It depends on add-core-tools-and-mcp (archived), on use-idp-tokens-for-mcp (7/8 tasks, not archived: its user-token authentication mode and its Catalog Entry Rules text are the base of this change) and on add-policy-and-sandbox (24/28 tasks: policy.tools.rules and policy.tools.risk decide approvals). This change must be archived after use-idp-tokens-for-mcp. The roadmap in openspec/config.yaml lists items 1 to 38 only; numbers 39 to 42 are missing, so the number 43 is used as given.
- **Code (not written by this change):**
  - control-plane/src/mcp: the `api` kind, the auth modes, the bundle entry and the rename of the admin view.
  - control-plane/src/api-connectors (new): parsing, rules, selection, calls and audit.
  - control-plane/admin/src/views/mcp.tsx and App.tsx: the Connectors view and its labels.
  - client/src/mcp and client/src/app: the Connectors settings page and the status bar labels.
  - client/src/api-connectors (new): the tool offer and the call routing.
  - client/src/ui/strings/en.json, de.json, cs.json: about 25 keys in each language. The generated bundles in client/build are not edited.
- **Data:** a new table for uploaded API documents and their operation list. Existing catalog rows and tables are unchanged. Existing entries get kind `mcp`.
- **Policy:** no new top-level policy key. API connectors are delivered in `policy.mcp.servers` with `kind: "api"`, and their calls are decided by `policy.tools.rules` and `policy.tools.risk`.
- **Glossary (owned by the product owner; not edited here):** see design.md, section "Glossary changes".
