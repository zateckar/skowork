# Spec Delta

## Purpose

Defines how the client connects to approved MCP servers, exposes their tools to the agent under policy control, and reports their health to the user.

## ADDED Requirements

### Requirement: Catalog-Only Servers
The client SHALL connect only to MCP servers listed in the user's effective catalog. control-plane/policy-service delivers that catalog inside the Policy bundle, built from control-plane/mcp-catalog Group assignments. It MUST NOT start or connect to any other MCP server, including servers configured by the user or added by skills, unless they are also in the catalog.

#### Scenario: Unlisted server
- **WHEN** a skill declares an MCP server that is not in the user's catalog
- **THEN** the server is not started, and the skill's dependency is reported as unavailable

### Requirement: Transports
The client SHALL support the stdio transport for local servers and the Streamable HTTP transport for remote servers, following the MCP specification version declared in the catalog entry.

#### Scenario: Remote server over HTTP
- **WHEN** a catalog entry describes a remote server with a URL
- **THEN** the client connects to it over Streamable HTTP with TLS validated against the Windows certificate store

### Requirement: Pinned Local Packages
The client SHALL start a local MCP server only from a package whose SHA-256 hash matches its catalog entry, checked on every start, using the arguments and environment from the catalog. The first argument is the executable, a path relative to the package root. The server runs as a process created by client/tool-sandbox.

#### Scenario: Hash mismatch
- **WHEN** the local package on disk does not match the catalog hash
- **THEN** the server is not started, the package is re-fetched from the catalog source, and a security event is recorded if the mismatch persists

### Requirement: Local Server Resource Limits
Each local MCP server SHALL run in its own client/tool-sandbox containment group with the limits policy.tools.mcp.limits.memory_mb, cpu_percent and processes (defaults 1024, 50 and 32). The wall-clock limit of client/tool-sandbox does not apply to a server process; Server Lifecycle governs how long it runs. Exceeding a limit terminates the server, and this counts as a crash.

#### Scenario: Long-running server
- **WHEN** a local server has been in use for longer than policy.tools.shell.limits.wall_minutes
- **THEN** it keeps running until it has been idle for policy.mcp.idle_timeout_minutes

### Requirement: Remote Server Authentication
The client SHALL authenticate to each remote MCP server with an audience-scoped token obtained from control-plane/identity for that server, and MUST NOT send the general Skowork access token to MCP servers.

#### Scenario: Token scoped to server
- **WHEN** the client calls a remote MCP server
- **THEN** the bearer token presented has that server as its only audience

### Requirement: Tool Namespacing And Filtering
The client SHALL expose each MCP tool to the agent under the id mcp:<server>/<tool>. It applies the include and exclude lists from the catalog entry and offers only tools that pass them. Decisions about individual MCP tools still come from client/policy-enforcement, for example policy.tools.rules entries such as `mcp:jira-corp/*`.

#### Scenario: Excluded tool
- **WHEN** the catalog entry for server "jira-corp" excludes the tool "delete_issue"
- **THEN** mcp:jira-corp/delete_issue is never offered to the model

### Requirement: Tool Risk Classes From Catalog
Each MCP tool SHALL take its risk class from the catalog's annotation for that tool, and default to external when none is given. A server's own tool annotations MUST NOT lower the risk class set by the catalog.

#### Scenario: Server claims read-only
- **WHEN** a server marks a tool as read-only but the catalog has no annotation for it
- **THEN** the tool is treated as risk class external

### Requirement: MCP Results Are Untrusted
The client SHALL return MCP tool content to client/agent-runtime as untrusted, with the server, tool and item identifier as provenance. client/agent-runtime applies the tool-result cap. client/policy-enforcement guarantees the content cannot grant permissions.

#### Scenario: MCP result provenance
- **WHEN** mcp:confluence-corp/get_page returns a page
- **THEN** the content enters the context as untrusted with provenance "mcp:confluence-corp/get_page" and the page id

### Requirement: Server Lifecycle
The client SHALL start local servers on first use, and stop them after policy.mcp.idle_timeout_minutes of inactivity (default 10). It restarts a crashed server at most three times per hour, and then marks it failed.

#### Scenario: Crashing server
- **WHEN** a local server crashes for the fourth time within an hour
- **THEN** it is marked failed, its tools are withdrawn from the agent, and the status bar shows the failure

### Requirement: Health And Diagnostics
The client SHALL show each catalog server's state in settings (Connections) and in the status bar: connected, idle, failed or unavailable, plus the number of usable tools. A server that connects but exposes no usable tools MUST be reported as a warning with the reason.

#### Scenario: Server with no usable tools
- **WHEN** a server connects but all its tools are excluded by policy
- **THEN** Connections shows the server with a warning "no usable tools" and names the policy exclusion as the reason

### Requirement: Catalog Updates Applied Live
When the catalog changes, the client SHALL apply additions, removals and updates without a restart. Removed servers are stopped, and their tools are withdrawn before the next model call.

#### Scenario: Server removed centrally
- **WHEN** an administrator removes a server from the user's Groups
- **THEN** after the policy change is received, the server is stopped and its tools are no longer offered
