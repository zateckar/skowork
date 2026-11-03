# Proposal

## Why

An agent is useful only if it can act. Users need the agent to work with their files, run commands, fetch web content and use organization systems such as Jira, Confluence and GitLab through MCP servers. All of this must stay inside centrally managed limits. This change adds the general-purpose tools and a catalog-driven MCP client, which together cover most everyday tasks in the MVP.

## What Changes

- Add the core tools:
  - `files.read`, `files.list`, `files.glob`, `files.grep`, `files.write`, `files.edit`, all subject to policy.filesystem rules;
  - `shell.run`, which runs Windows PowerShell 5.1 by default, or pwsh, per policy.tools.shell.executable, inside client/tool-sandbox according to policy.tools.shell.mode;
  - `web.fetch`, which follows policy.network;
  - managed downloads into a quarantine folder;
  - text extraction for docx, xlsx, pptx and pdf in a sandboxed worker, reused later by client/mail-calendar (attachments) and client/knowledge-graph.
- Add the MCP client:
  - stdio and Streamable HTTP transports;
  - connects only to servers in the user's catalog;
  - local server packages are hash-pinned and run in the sandbox;
  - tools are namespaced `mcp:<server>/<tool>`, with per-server include/exclude and catalog-provided risk classes (default: external);
  - remote servers are called with audience-scoped tokens;
  - clear health and error reporting, including when a server exposes no usable tools.
- Add the control-plane MCP catalog: approved remote and local MCP servers assigned to Groups, with tool risk annotations and versioning. Changes reach clients through control-plane/policy-service.

Out of scope:
- How the allow/approve/deny decision is made (client/policy-enforcement).
- How processes are isolated (client/tool-sandbox).
- Browser and computer-use tools (separate changes).
- MCP servers that run inside the control plane.
- MCP sampling and elicitation (deferred).

## Capabilities

### New Capabilities
- `client/core-tools`: file, shell, web-fetch and download tools available to the agent
- `client/mcp-client`: connecting to catalog MCP servers and exposing their tools to the agent
- `control-plane/mcp-catalog`: central registry of approved MCP servers per Group, including tool risk annotations

### Modified Capabilities
- `control-plane/identity`: adds audience-scoped token exchange and exchanged-token signing key publication for remote MCP servers. These requirements moved here from add-client-foundation.

## Impact

- **Roadmap:** change 5 of 13 (MVP). It depends on:
  - add-client-foundation (storage, identity, Identity Provider Trust);
  - add-agent-runtime (tool registration, rationale argument, tool-result cap, provenance);
  - add-policy-and-sandbox (decisions, filesystem/network rules and canonicalisation, sandboxed processes, shell mode/limits/timeouts, egress proxy, Policy bundle delivery of the catalog);
  - add-work-inbox (the Connections settings section and status-bar health that client/mcp-client reports into).
- **Forward references:**
  - control-plane/admin-console (change 7) hosts the catalog administration UI.
  - client/skills (change 9) declares required MCP servers that client/mcp-client resolves.
  - client/mail-calendar (change 11) reuses the document extraction for attachments.
  - client/knowledge-graph (change 12) reuses the document extraction.
- **New code:**
  - `client/` tool modules and the MCP client;
  - `control-plane/` catalog service and its admin API.
- **Dependencies:**
  - a JSON-RPC/MCP protocol implementation in C++ (in-house; see design);
  - Windows PowerShell 5.1, which ships with Windows, as the default shell. pwsh is used only where policy.tools.shell.executable selects it and it is installed.
- **Security review:** document parsers, shell sandbox defaults, and MCP package pinning.
