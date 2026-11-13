# Design

## Context

- **Tool plumbing:** tools register with the agent runtime (add-agent-runtime). Every call passes through the `BeforeToolCall` policy hook (add-policy-and-sandbox).
- **Sandbox:** sandboxed processes and the egress proxy come from client/tool-sandbox. This change only consumes them.
- **Tokens:** audience-scoped tokens are issued by control-plane/identity. This change adds the Audience-Scoped Token Exchange requirement to that capability (specs/control-plane/identity), because remote MCP servers are its first consumer.

## Goals / Non-Goals

**Goals:**
- A small, predictable tool set that covers day-to-day file, shell and web work.
- MCP as the single extension mechanism for organization systems, fully governed by the catalog.

**Non-Goals:**
- A user-editable `.mcp.json`. Users request servers through the catalog process instead.
- MCP sampling, elicitation and resource subscriptions (later change).
- Python or Node runtimes bundled with the client. A local server package must be self-contained.

## Decisions

### D1. Windows PowerShell 5.1 as the default shell
- **Choice:** policy.tools.shell.executable selects `powershell` or `pwsh`, and the default is `powershell`, which is Windows PowerShell 5.1 and present on every Windows 11 machine. Either shell is started with `-NoProfile -NonInteractive`, and the command is passed through `-EncodedCommand`. If `pwsh` is selected but not installed, shell.run is not offered and the status shows the reason.
- **Running a command.** The command runs with a short prelude: progress records are switched off, and console output is set to UTF-8. The result gives the exit code, stdout and stderr. A non-zero exit code makes the call a failed call. When a limit stops the command, the result names the limit, says whether it came from the call's `timeout_seconds` or from the policy limit, and keeps the output captured so far. A missing `pwsh` is reported through `PolicyEnforcer`'s unavailable-tool callback, which hides shell.run and denies its calls. The effective-policy summary shows the reason.
- **Destructive patterns.** policy.tools.shell.destructive_patterns are ECMAScript regular expressions. Each is matched case-insensitively anywhere in the command. The built-in patterns always apply and cover:
  - recursive deletion (Remove-Item and its aliases with `-r…` or `/s`);
  - disk formatting and partitioning;
  - registry deletion;
  - changes to accounts, groups, the firewall, Defender, ACLs, the execution policy and boot settings;
  - a nested PowerShell started with an encoded command, which cannot be inspected.

  An invalid policy pattern is skipped. A pattern that is too complex to evaluate counts as a match.
- **Alternatives considered:**
  - cmd.exe: too limited for the agent.
  - PowerShell 7 (`pwsh`) as the default: not installed everywhere. It is available as an option for Groups whose machines have it.
  - Git Bash: not installed for most users.

### D2. File access through the policy-enforcement path API
- **Choice:** file tools never resolve paths themselves. They call the canonicalising path check of client/policy-enforcement (add-policy-and-sandbox design D4). That check returns an opened handle, or a parent-directory handle for creates, and the tool operates only on that handle. This keeps one implementation of the junction, 8.3 and ADS rules and closes the TOCTOU gap.
- **Writes** go to a temporary file created in the opened, checked parent folder. The temporary file is then renamed over the target relative to that folder handle (`NtSetInformationFile` FileRenameInformation with a RootDirectory), which makes the write atomic and keeps it off any path that could be swapped. `ReplaceFileW` was the earlier plan; it was dropped because it takes paths. A replaced file gets the folder's inherited permissions, not its old ones.
- **Encodings.** files.read decodes UTF-8 (with or without BOM), UTF-16 with a BOM, and otherwise Windows-1252. files.edit writes the file back in the encoding it found. When the old text uses `\n` and the file uses `\r\n`, the match is retried with `\r\n`.
- **Walks.** files.glob and files.grep check every entry with the path rules and count the denied ones. They do not follow linked folders, and stop after 50,000 entries. grep skips files larger than 8 MB and binary files.
- **Alternative considered:** each tool canonicalising on its own. Rejected because duplicate implementations drift.

### D3. In-house MCP client in C++
- **Choice:** a minimal JSON-RPC 2.0 and MCP client (initialize, tools/list, tools/call, notifications, Streamable HTTP session handling) on WinHTTP. It uses the same JSON parser as the rest of the client, and is fuzzed in CI.
- **Alternative considered:** an official MCP SDK. At decision time there is no maintained C++ SDK, and bundling the Python, TypeScript or C# SDK would break the footprint budget.

### D4. Text extraction (owned by this change)
- **HTML:** converted to Markdown-ish text with a small in-house DOM-less extractor (a tokenizer over tags, entities and text), stripping scripts, styles and hidden elements. It has its own fuzz target.
- **Office and PDF:** this change owns the docx, xlsx and pptx extractors (miniz for the zip container plus pugixml over the OOXML parts) and the PDF extractor (PDFium text API, BSD licence). PDFium is the prebuilt `pdfium.dll` from bblanchon/pdfium-binaries, loaded with `LoadLibrary` only by the extract worker; without the DLL a PDF extraction fails with a clear reason. miniz, pugixml and the PDFium build are vendored with pinned hashes in client/third_party/README.md. Each document is extracted by its own `skowork-worker.exe --role extract --kind <kind>` process, started through the client/tool-sandbox process API. The process gets a restricted token, no network, one process, and the extraction worker's memory budget from client/desktop-host Worker Memory Budgets (1 GB). It has a 30-second time limit. The document goes in on stdin and its text comes back on stdout. A crash, hang or limit hit ends only that process, and the tool gets an extraction error.
  - **Limits inside the worker:** at most 20,000 zip entries, 64 MB per uncompressed part, 256 MB in total per document, and 32 MB of text. The XML walks are iterative, so deep nesting cannot exhaust the stack.
  - **Output structure:**
    - docx: `#` headings (Title, Heading n, outline levels), `- ` list items, and table rows as cells joined by ` | `.
    - xlsx: `## Sheet: name`, then rows of cells joined by ` | `, with skipped columns kept as empty cells.
    - pptx: `## Slide n` in presentation order, with bulleted paragraphs as list items.
    - pdf: `## Page n`. client/mail-calendar (change 11) uses it for attachment text, and client/knowledge-graph (change 12) reuses the same worker role for indexing. The `extract` role is added to the worker roles listed in add-client-foundation design.
- **Alternatives considered:**
  - Headless Chrome rendering for every fetch: too heavy. browser-use covers pages that need JavaScript.
  - Windows IFilter: availability depends on installed Office and Adobe components, and it runs in-process.
  - lexbor for HTML and minizip plus zlib for the zip container (the earlier plan): rejected on 2026-10-22 to keep the vendored set small. Text extraction needs no full HTML5 tree, and miniz is a single file that covers inflate.
  - Building PDFium from source: rejected, because its build needs the Chromium toolchain.

### D5. Catalog storage and package endpoint
- **Choice:** SQLite tables (`node:sqlite`, in the one control-plane process) for entries, versions and tool snapshots. Packages live in content-addressed storage on the control plane's disk (`/mcp/packages/{sha256}`), served with bearer-token auth.
- **Alternatives considered:**
  - Clients downloading from upstream registries (npm, PyPI): rejected because of supply-chain risk and internet egress.
  - PostgreSQL for the catalog and package bytes (the earlier plan): rejected on 2026-10-22 with the control-plane stack decision; a few tens of MB per package fits in SQLite or on disk.

### D6. Catalog service decisions
- **Storage.** Tables `mcp_servers` (the current entry), `mcp_server_versions` (a snapshot of the entry and its Groups after every change; the highest id is the catalog stamp), `mcp_assignments`, `mcp_tools` (the discovery snapshot and the review state of each tool) and `mcp_packages` (the package bytes, addressed by SHA-256).
- **Discovery.** For a remote entry the control plane connects over Streamable HTTP, runs `initialize`, sends `notifications/initialized`, and pages through `tools/list` (JSON and event-stream answers are both read). For an audience-token entry it presents a short-lived token for the entry's audience, signed by the identity key, with the subject `skowork-catalog-discovery`. Plain HTTP is accepted only when the control-plane server setting `mcp_catalog.allow_insecure_http` is set. A local entry cannot be discovered by the server, so its tools are limited only by the include and exclude lists.
- **Review.** The first successful snapshot is the baseline and counts as included. A tool that appears later is `pending`, and a tool that the server stops reporting is marked absent but keeps its decision. The client receives `exclude_tools` = the configured exclude list plus every pending or excluded tool, and `include_tools` = the configured include list; including a tool that the include list would not match adds it to a non-empty include list.
- **A failed discovery** stores the entry disabled; the first successful discovery is a precondition for enabling it.
- **Delivery.** The catalog is the source that control-plane/policy-service reads for policy.mcp.servers. It caches the enabled entries for a few seconds (control-plane server setting `mcp_catalog.cache_seconds`) with invalidation on change. Its stamp is part of the bundle compile cache key, so any catalog change reaches every affected bundle.
- **Change notification.** After each change the catalog asks the policy service to send policy-changed to the Groups the entry was assigned to before and after. The catalog sends no push message of its own.
- **Token exchange.** control-plane/identity asks the catalog which audiences a user may obtain a token for (Remote Audience Authorization). The static control-plane setting `identity.mcp_audiences` is also honoured. The client calls `POST /api/v1/identity/token-exchange` with `{audience}`. The exchanged token carries `sub`, `preferred_username`, the single audience and the presented token's expiry, and is signed with the `mcp-token` key ring. The keys are published at `/.well-known/skowork-token-keys.json` with the issuer, which is the control plane's public base URL. A platform-admin rotates them with `POST /api/v1/admin/identity/token-keys/rotate`. Policy bundles and exchanged tokens use the same key-ring code, but with separate keys.
- **Admin API.** Everything is under `/api/v1/admin/mcp` and restricted to platform-admin:
  - `servers`: list and create;
  - `servers/{id}`: get, update (a partial entry, `groups`, `enabled`) and delete;
  - `servers/{id}/discover`: run discovery again;
  - `servers/{id}/versions`: the version history;
  - `servers/{id}/tools/{tool}`: the review decision, `included` or `excluded`;
  - `packages`: list and upload.

  Every update of a remote entry runs discovery again. A failed discovery disables the entry, and a request to enable it is refused with 409.
- **Packages.** Uploaded with `PUT /api/v1/admin/mcp/packages`, served from `GET /api/v1/mcp/packages/{sha256}`. Storing the bytes in SQLite is acceptable for the expected package sizes (a few tens of MB); the size limit is the control-plane server setting `mcp_catalog.max_package_bytes`.
### D7. web.fetch and managed downloads
- **Requests.** web.fetch sends GET through WinHTTP with redirects followed by hand: client/policy-enforcement checks the first URL and every redirect target before connecting. A redirect from https to plain http is refused. web.fetch is the only client request that may use plain `http://` to any host. It uses its own WinHTTP session with a browser-like User-Agent, and bodies are limited to 64 MB.
- **Integrated authentication.** It is decided per request and per redirect hop. Only for a host matching policy.network.integrated_auth_hosts (the same host patterns as the egress lists) does the request use the low autologon level and answer a 401 Negotiate/NTLM challenge once with the logged-on user's credentials. For every other request WinHTTP keeps the high autologon level. For loopback hosts automatic authentication is also switched off, because WinHTTP was observed to start a Negotiate handshake with a loopback server even at the high level. Loopback never goes through a proxy, so proxy 407 handling is unaffected.
- **Results.**
  - The text starts with header lines: URL (the final one after redirects), Status, Content-Type and, for HTML, Title.
  - The body is converted by type:
    - HTML goes through the D4 extractor.
    - docx, xlsx, pptx and pdf (detected by content type or file name) go through the extract worker.
    - Other text is decoded as files.read decodes it.
    - format `raw` returns the decoded body, cut at max_bytes (default 256 KB, at most 4 MB).
  - A non-2xx status is a failed call that still returns the body.
  - Provenance is `web`, with the final URL and the fetch time, and is untrusted.
- **Managed downloads.**
  - **What is a download:** a binary body, or a document that cannot be extracted.
  - **Where it is saved:** `<data root>\quarantine\<UTC timestamp>-<safe name>`.
    - The name comes from Content-Disposition or the URL path, with reserved characters, device names and path separators neutralised.
    - An existing file is never overwritten.
  - **Mark-of-the-web:** the `Zone.Identifier` stream (ZoneId=3, HostUrl, ReferrerUrl after a redirect) is written before the content.
  - **What the result returns:** only the path.
  - **No opening:** the data root is denied to every tool, so the agent cannot open the file.
  - **No running:** shell.run classifies any command that names the quarantine folder, or contains the word "quarantine", as destructive. It is therefore denied by default, and runs only with the user's approval where policy.tools.risk allows destructive calls with approval.
  - **Getting the file:** the user moves it out of the quarantine to use it.
  - **Known limit:** the textual check does not catch a command that builds the path indirectly. In sandboxed mode the restricted token is the next barrier, and the data root's DACL grants only the user and SYSTEM.
- **Alternative considered:** saving downloads in the workspace with mark-of-the-web only. Rejected, because the agent could then open and run them without any decision.

### D8. MCP client structure and lifecycle
- **Layers.**
  - `wb_mcp_core` is portable and fuzzed (`fuzz_mcp`). It covers JSON-RPC messages, newline framing for stdio, the initialize, tools/list and tools/call payloads, catalog entry parsing, the include/exclude match (the catalog's rule: `*` is any run of characters, case-sensitive, exclude wins), and the conversion of call results to text.
  - The transports and the McpManager are Windows-only.
- **Untrusted input from servers:**
  - Messages are capped at 16 MB.
  - Descriptions are capped at 4,000 characters.
  - Tool names must match `[A-Za-z0-9_.-]{1,64}`; other tools are skipped.
  - At most 50 tools/list pages are read.
  - Server requests other than `ping` get -32601, because sampling, elicitation and roots are non-goals.
- **Stdio.**
  - The process comes from the new long-lived process API of client/tool-sandbox (`Sandbox::start`). It shares the job, restricted token, scrubbed environment and egress-proxy setup with `Sandbox::run`, but has no wall-clock limit.
  - stderr is kept as an 8 KB tail for diagnostics.
  - A cancelled or timed-out call sends `notifications/cancelled`.
- **Streamable HTTP.**
  - Every message is a POST with `Accept: application/json, text/event-stream`. An SSE reply is read until the matching response.
  - `Mcp-Session-Id` and `MCP-Protocol-Version` (the version the server negotiated) are sent on later requests, and the session is deleted on close.
  - Redirects are not followed, and the client/policy-enforcement egress check runs before every request.
  - A 401 fetches a fresh token once. A 404 for a known session starts a new session and repeats the call once.
- **Tokens.** These are audience-scoped tokens from `POST /api/v1/identity/token-exchange`, cached per audience until 60 seconds before `expires_at`. An entry with auth `none` sends no Authorization header. The general Skowork access token is never sent to an MCP server.
- **Packages.**
  - The zip is kept at `<data root>\mcp\packages\<sha256>.zip`. Its hash is checked on every start, and the bytes that were checked are the bytes extracted.
  - On a mismatch the file is deleted and fetched again from `GET /api/v1/mcp/packages/{sha256}`. A fetched package that still does not match records the security event `mcp_package_mismatch`, and the server stays unavailable.
  - The package is extracted fresh to `<data root>\mcp\run\<server>\`. Entry names must be safe relative paths, with at most 10,000 files and 1 GB.
  - `args[0]` is the executable, relative to the package root; the remaining arguments are quoted for the Windows command line.
  - The process runs restricted, with network through the egress proxy and the policy.tools.mcp.limits limits.
- **Tools.**
  - Each tool is registered as `mcp:<server>/<tool>`. Its risk class comes from the entry's `risk` map, else external. Its parameters are the server's inputSchema.
  - A tool excluded by the catalog, or denied by a policy.tools.rules entry, is not registered. A server whose tools are all filtered out gets the warning "no usable tools" with the first reasons.
  - The `rationale` argument of the runtime is removed before the call.
  - Results are untrusted, with the provenance kind `mcp` and the id `<server>/<tool>[#<item>]`, which the Thread and the untrusted-data delimiter show as `mcp:<server>/<tool>#<item>`. The item is the first resource URI in the result, else an id-like argument.
- **Tool list cache.** The listed tools are cached in `<data root>\mcp\tools-<server>.json` together with the entry's fingerprint, so the tools register at startup without starting the server. Without a matching cache, a background discovery starts the server once.
- **Lifecycle.**
  - **States:** connected, idle, starting, failed and unavailable.
  - **Start and stop:** a server starts lazily on the first call and stops after policy.mcp.idle_timeout_minutes without calls.
  - **Crashes:** a crash (the process ending by itself, or a limit hit) leaves the server idle, and it restarts on the next call. The fourth crash within an hour marks it failed and withdraws its tools until the catalog entry changes.
  - **Retries:** an unavailable server (unreachable, refused token, blocked by egress, bad package) is retried every 5 minutes.
- **Catalog updates.** The manager re-applies policy.mcp.servers on every policy change. Removed or changed entries (compared by their JSON) have their tools withdrawn and are stopped before the call returns. Unchanged entries re-evaluate their tools against the new policy. The agent runtime registry keeps withdrawn definitions alive, so a call already in flight finishes, and it offers the change from the next model call.
- **Health.** `app.status.mcp` lists `{id, name, kind, state, tools, usable_tools, warning, error}` for each server. The status bar shows "Connections n/m" or the failed servers. Settings shows a Connections card.
- **Test servers.**
  - `dev/mcp-test-server` (dev-only, never shipped) uses the official `@modelcontextprotocol/sdk` over stdio and Streamable HTTP. It is the reference server for the transport tests and the mock Jira of the end-to-end test.
  - A small C++ stdio server (`mcp_test_server`) is packaged by the tests for the package, crash and lifecycle cases.

## Risks / Trade-offs

- [Prompt injection through fetched pages or MCP results leads to harmful tool calls] → All such content is marked untrusted with provenance (client/agent-runtime). Approval cards show the provenance (client/work-inbox). Egress is limited by policy.network (client/policy-enforcement).
- [Document parsers are exploited by crafted files] → Parsers run in the sandboxed extract worker, are fuzzed in CI, and are killed on time and memory limits.
- [Local MCP servers need runtimes we do not ship] → The catalog accepts only self-contained packages (single exe, or a portable runtime inside the package). The catalog review checks this.
- [Destructive shell detection by pattern is incomplete] → Patterns are one signal only. The sandbox limits the blast radius, and policy can require approval for all of shell.run.
- [Tool-list drift on remote servers] → The discovery snapshot and the review-before-enable rule.

## Open Questions

- Which organization MCP servers are in the first catalog (for example Jira, Confluence, GitLab)? This affects only catalog content, not the specs.
