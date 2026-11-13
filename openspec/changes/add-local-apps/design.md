# Design

## Context

The client runs tools in child processes contained by client/tool-sandbox (job objects with limits, reduced token, egress through the child-process proxy with deny-all for No-Network Mode). The UI has no web engine, so apps open in the user's default browser. The product owner chose a bundled Node.js runtime and live MCP data through the client (2026-10-31).

## Decisions

### D1. Folder layout
- **Choice:** `%LOCALAPPDATA%\Skowork\apps\<app-id>\` with `app.json` (`{name, description, entry, mcp_tools: ["mcp:<server>/<tool>"]}`), `server.js` (entry), `public\`, and `data.db` (node:sqlite). The app id is a slug of the name, unique per user.
- **Alternatives considered:** apps inside the agent workspace (mixed with other work files; harder to list and delete).

### D2. Runtime and containment
- **Choice:** the optional package holds a pinned, signed `node.exe` (Node.js 26 LTS, which has node:sqlite and network permission), its LICENSE and the client's app host script. Apps run as `node --permission --allow-fs-read=<app folder>,<runtime> --allow-fs-write=<app folder> --require <runtime>/app-host.js <entry>` (no `--allow-net`, `--allow-child-process`, `--allow-addons`, `--allow-ffi` or `--allow-worker`), inside client/tool-sandbox with No-Network Mode and a 512 MB memory limit. The app calls `http.createServer(...).listen(process.env.SKOWORK_APP_PIPE)` as usual; the app host turns that listen into connections framed over the process's stdin and stdout, so the app opens no socket or pipe and nothing but the client reaches it. The app's console output is framed the same way and becomes its log.
- **Validation:** spike 1.1 checks that node:sqlite works under the permission model, that listening on a named pipe works, that fetch to an internet host fails, and measures start time and memory on the baseline hardware.
- **Spike 1.1 results (2026-11-01, development workstation, official win-x64 zips):**
  - Node.js 24.22.2 LTS: node:sqlite works with `--permission` limited to the app folder; reads and writes outside it and child processes are refused (ERR_ACCESS_DENIED); listening on a named pipe works. But the permission model of Node.js 24 has no network scope: a TCP connection to a loopback port succeeds under `--permission`, and No-Network Mode only refuses traffic that goes through the egress proxy, so app code could reach loopback services and, on laptops without endpoint filtering, the internet. This disproves the egress assumption for Node.js 24.
  - Node.js 26.13.0 LTS (LTS since 2026-10-28) adds `--allow-net`. Without it, fetch, TCP connections (loopback included) and listening, also on a named pipe, all fail with ERR_ACCESS_DENIED, and the app keeps running. Hence the stdio transport above, with the network refused by the runtime itself.
  - With the stdio app host: node:sqlite read and write in the app folder, an http.Server answering framed requests, fetch to https://example.com refused. Ready 29 ms after spawn; resident memory about 49 MB (Node.js 24, same script); node.exe is 95.5 MB (Node.js 26.13.0), within the 100 MB package budget.
  - On this workstation endpoint security already refuses connections from the unsigned-path node.exe (EACCES), so the internet case was checked through the permission error, not through the network.
- **Alternatives considered:** the user's own Node.js (missing or different versions on laptops; rejected by the product owner); serving static files from the client only (no server logic, which the product owner asked for).

### D3. Owner-only serving
- **Choice:** the client runs one loopback HTTP listener (127.0.0.1, random port) that maps `/<app-id>/...` to the app's framed connection (one request per connection, the `/<app-id>` prefix removed). apps.open starts the app and opens `http://127.0.0.1:<port>/<app-id>/?launch=<single-use token>`; the listener swaps the token for an HttpOnly, SameSite=Strict session cookie scoped to the app path and redirects. Requests without a valid cookie get 403. The listener rejects Host headers other than the loopback address (DNS rebinding).
- **Alternatives considered:** each app on its own TCP port (any local user could connect; app code would have to check tokens).

### D4. MCP bridge
- **Choice:** the listener answers `POST /<app-id>/_skowork/mcp/<server>/<tool>` for the app's pages (same cookie) with the JSON result. It checks the manifest list, the user's confirmation (stored with a hash of the list), the tool's read risk class from the catalog, and policy.apps.mcp, then calls through the MCP client and client/policy-enforcement with origin `app:<id>`. Calls are recorded in the approval log like the agent's.
- **Alternatives considered:** a bridge reachable from the app's server process (it would need network or pipe access to the client; the page-side bridge keeps the process without any channel out).

### D5. Lifecycle and tools
- **Choice:** an app manager in the client starts apps on open, stops them after 15 minutes without requests or at shutdown, keeps a 1 MB ring log of stdout and stderr, and enforces 3 running apps (the least recently used is stopped). apps.create grants the Run write access to the new app folder for its duration under client/policy-enforcement. The Apps view is a top-level navigation entry.
- **Alternatives considered:** keeping apps running (memory: each Node process takes about 40 MB).
- **Implementation notes (2.1–2.6):**
  - apps.create with an `app` argument changes the manifest of an existing app and grants the Run its folder again, so the agent can change an app in a later Thread. A Run's grant lasts while the Run is queued, running or interrupted.
  - Confirmations, last use and logs are kept in `apps-state\` next to `apps\`, out of the apps' reach.
  - apps.open asks the user about the app's MCP tools with a Question (Allow / Don't allow) and resumes; the Apps view asks with a dialog. A changed list asks again.
  - The bridge runs the catalog's tool definition only when client/policy-enforcement allows the call: no one can approve a call from an app page, so an approval decision refuses it.
  - "Change with the agent" in the Apps view asks what should change and starts a new Thread with that request, naming the app and its id.
  - Runtime package: `dev/scripts/fetch-node-runtime.ps1` downloads the pinned zip, checks its SHA-256 and the Authenticode signature, and keeps node.exe, LICENSE and VERSION in `client/runtime/node` (ignored by git). The build lays out `bin\runtime\node` (node.exe, LICENSE, app-host.js) and fails above 100 MB (binary megabytes, like the core package's footprint check). Measured: 100,261,172 bytes (95.6 MB).
  - Release: the release workflow fetches the runtime with the same script before the build. `client/tools/package.ps1` writes the optional package next to the desktop zip as `Skowork-<version>-app-runtime-win-x64.zip`, holding `runtime\node` (node.exe, LICENSE, app-host.js) to be extracted next to Skowork.exe, with its own SHA-256 file (38 MB compressed).

## Risks / Trade-offs

- [App code written by the agent is wrong or hostile] → It runs without network, outside other folders, without child processes, under memory limits, and only the user reaches it.
- [An app leaks MCP data] → It can only read tools the user confirmed, has no network, and is served only to the user.
- [The Node.js permission model has gaps] → It is one layer; the sandbox's containment and No-Network Mode stay in place; the runtime version is pinned and updated with the client.
- [Installer size] → The runtime is an optional package with its own budget.

## Acceptance notes (2026-11-01)

- With the real organization LLM, on a development client signed in through Keycloak:
  - A request for a 5-slide presentation produced the "Skowork Benefits Presentation" app. It opened in the browser, and the arrow keys move between the slides.
  - A request for a live view over the connected mock Jira MCP server produced the "Jira Issues Live Board" app. After the user confirmed its MCP tools, it showed DEMO-101 to DEMO-103 through the bridge, and search and the status filter work.
  - The user checked both pages in the browser.
- Found and fixed: an MCP server whose discovery had failed, for example because egress policy refused it, stayed unavailable after the policy allowed it. It is now discovered again when the policy changes (McpHttp test).