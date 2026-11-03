# Design

## Context

This design builds on:
- add-client-foundation: identity, the fleet-registry push, and DPAPI-encrypted storage.
- add-agent-runtime: the mandatory `BeforeToolCall` hook, Risk classes, and Approval interrupts.

The client runs as the user without admin rights. That rules out kernel drivers, WFP callouts and machine-wide firewall changes from inside Workbench. Enforcement therefore has three layers:

1. **App layer:** client/policy-enforcement. It sees 100% of agent-initiated tool calls.
2. **Process layer:** client/tool-sandbox. Job Objects, a restricted token and the egress proxy.
3. **OS layer:** firewall, WFP and WDAC rules deployed by endpoint management for Groups that need hard guarantees. This layer is outside Workbench. It is documented here and reported as an enforcement level.

## Goals / Non-Goals

**Goals:**
- One deterministic decision function, `decide(tool_call, context, policy) → allow|approve|deny`, that is unit-testable and identical for attended and unattended Runs apart from the `unattended` rules.
- Safe defaults: fail closed, deny unknown external and destructive calls, and make every escape path explicit and tested.
- An administrator workflow (author, validate, simulate, stage, publish, roll back) with a full audit trail.

**Non-Goals:**
- A general-purpose sandbox that contains a malicious native binary. The sandbox reduces blast radius and enforces policy for cooperative tooling. It is not a malware container.
- Per-user policy exceptions. Exceptions are modelled as small Groups.

## Decisions

### D1. Policy bundle format and signing
- **Format:** a compact JWS (ES256) whose payload is the effective policy as JSON. Claims: `sub` (UPN), `ver` (monotonic integer per user), `iat`, `exp` (default 7 days), `groups`, and `src`, a map of rule paths to their source Group, used for "why denied".
- **Signing key:** a dedicated policy signing key, separate from the token key. Compromising one does not forge the other. Public keys are published at `/.well-known/policy-keys.json`.
- **Client checks:** the signature, that `sub` equals the bound user, `exp`, and that `ver` is not lower than the current version. Bundles are cached in `app.db`, encrypted.
- **Alternatives considered:**
  - Unsigned policy over TLS only: a cached copy on disk could be edited.
  - Policy embedded in the access token: tokens are too big and too short-lived for offline use.

### D2. Effective policy merge (server-side)
- **Order:** guardrails, then Group policies sorted by priority, highest priority first.
- **Lists** (allow lists, egress_allow, filesystem read and write): the union, minus anything matched by any deny in any Group or guardrail.
- **Decision maps** (`tools.rules`, `tools.risk`, `unattended.rules`): a key is `deny` if any Group sets it to deny. Otherwise it takes the value of the highest-priority Group that sets it, so a more permissive Group can grant `allow` over another Group's `approve`. Guardrails can force any key to `approve` or `deny`.
- **Scalars:** the first Group that sets the value wins, then the value is clamped by the guardrail's bound, for example `max_concurrent_runs ≤ guardrail`.
- **MCP servers** are not a policy list. They come from mcp-catalog Group assignments, which are unioned across the user's Groups and filtered by guardrails.
- **Server-side only.** The client never merges, so every client applies exactly what simulation showed.
- **Alternative considered:** most-restrictive-wins for scalars. Rejected because a user who is in both "standard" and "developers" would lose developer capabilities.

### D3. Decision function (client)
**Key layout.** Decisions and settings never share a namespace:
- `tools.rules`: a map from tool-id pattern to `allow|approve|deny`. Patterns support wildcards such as `browser.*` and `mcp:jira-corp/*`.
- `tools.risk.<class>`: the default decision for each Risk class.
- `tools.<group>.<setting>`: settings for a tool group (`shell`, `files`, `browser`, `computer`, `mail` and so on), including `tools.<group>.limits`. These are never decisions.
- `unattended.allowed_risk` and `unattended.rules`: the same shapes, applied only to Unattended runs.

**Decision steps:**
1. Look up the most specific rule: the most specific matching `tools.rules` pattern, then `tools.risk.<class>`, then the Risk Class Defaults in the spec.
2. Apply the argument-level checks: filesystem path rules for path arguments, and egress rules for URL arguments. If any check fails, the result is deny.
3. Apply the tool-declared floor (for example the mail.send approval) and, for a Skill's subagent, the Skill-declared floor. The result is the strictest of these.
4. If the Run is unattended, take the stricter of the result so far and the unattended rules. A Risk class missing from `unattended.allowed_risk` forces at least approve, unless `unattended.rules` explicitly allows that tool id. That explicit allow is also the only way a tool spec's named Unattended exception is expressed, for example mail.send.
5. The result is final. Untrusted provenance cannot lower it, and the runtime offers no code path that takes model or tool output as an approval.

### D4. Path canonicalisation
- **Steps:**
  1. `ExpandEnvironmentStringsW`.
  2. `GetFullPathNameW`.
  3. Reject `\\?\GLOBALROOT`, device paths, the `\\.\` prefix and any `:` beyond the drive letter (this covers ADS).
  4. Open with `FILE_FLAG_OPEN_REPARSE_POINT` and resolve with `GetFinalPathNameByHandleW`. The final path is the one checked.
  Step 3 also rejects reserved device names (CON, NUL, COM1 and so on) and control characters, and is repeated after `GetFullPathNameW`, which can turn such names into device paths. For a path whose tail does not exist yet, the deepest existing ancestor is resolved and the missing components are appended.
- **Reads:** after the check, the file is opened and the opened handle's final path is checked again, so a link swapped in after the check is caught.
- **Writes:** the parent directory's final path is checked, and the file is then created relative to the opened parent handle (`NtCreateFile` with a root directory and `FILE_OPEN_REPARSE_POINT`). This closes the time-of-check-to-time-of-use (TOCTOU) gap. A reparse point or a file with more than one hard link in the target's place is refused, because its other names may lie outside the allowed folders. Truncation happens only after these checks, by handle.
- **Rule folders are final paths too.** The workspace and data root are canonicalised like tool paths. A redirected profile folder (folder redirection, a packaged-app file system redirect, a junction under `%LOCALAPPDATA%`) would otherwise make the workspace unreachable, or make the data root look like an ordinary folder. A UNC path such as an administrative share names a different final path, so it never matches a local rule.
- **Globs:** a custom matcher with `**` and `*`, case-insensitive. A fuzz target covers the canonicaliser and matcher together.

### D5. Sandbox implementation
- **Job Objects:** one job per process the client starts. `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, with breakaway disallowed and no `SILENT_BREAKAWAY`. Also `JOB_OBJECT_LIMIT_JOB_MEMORY` (the memory limit covers the whole group, not each process), `ACTIVE_PROCESS`, and a hard-capped CPU rate. The process is created suspended, assigned to the job, then resumed. The job's completion port reports memory and process-count limit hits, and the client then terminates the job. When the started process exits, the job is terminated too, so nothing it left running survives the tool call.
- **Restricted token:** `CreateRestrictedToken` with `DISABLE_MAX_PRIVILEGE`, with the Administrators SID set deny-only. The token's owner and default DACL are set to the user (and SYSTEM): an elevated token owns new objects through Administrators, so with Administrators deny-only the child could not open its own process and would fail to start. The integrity level is Medium; Low was considered but breaks too many developer tools. Shell mode `allow` keeps the user's token but still uses the job and the scrubbed environment, without the proxy.
- **No-network mode (decided 2026-10-22):** the process is started like a sandboxed process with network access: job, restricted token, scrubbed environment and the egress proxy variables. Its proxy session is marked no-network, and the proxy refuses every destination for that credential with the rule `no-network`, before the egress rules are consulted. The refusals go to the approval log like other egress denials. Workbench does not rely on Windows Firewall or AppContainer for this mode. The enforcement level is the same as for all child traffic (D6): `proxy-only`, unless IT deploys the firewall profile for Workbench child processes.
- **Environment:** built from scratch. Only an allowlist of variables is kept (PATH, TEMP, USERPROFILE and similar), plus `HTTP_PROXY`/`HTTPS_PROXY` (and their lower-case forms) set to `http://workbench:<cred>@127.0.0.1:<port>`.
- **Output:** each stream is read continuously on its own thread. The first `output_kb` are kept and the rest is read and discarded, so the process never blocks on a full pipe. Decoding: UTF-8 when valid (a sequence cut at the limit is dropped), UTF-16LE when the bytes look like it, otherwise the OEM code page.
- **Shell modes:** the sandbox decides the process settings for `shell.run` (client/core-tools). In `sandboxed` mode the working folder must be writable under the filesystem rules, or the command is not started.
- **Alternatives considered:**
  - Windows Sandbox or Hyper-V: requires admin rights and the feature to be installed, and is heavy.
  - WSL: not available everywhere and outside the policy model.
  - AppContainer for all modes: loopback is blocked, so the proxy cannot be used, and developer toolchains break.
  - AppContainer for no-network mode only (implemented first, then rejected on 2026-10-22). AppContainer network isolation is enforced by Windows Firewall. On a workstation whose firewall profiles were all off, a container without capabilities still connected to loopback. Relying on it would make the mode depend on firewall configuration that Workbench does not control. It also needed a per-user profile registration and ACL grants on working folders.

### D6. Egress proxy
- **Lifetime:** an in-process asynchronous HTTP CONNECT and forward proxy, bound to `127.0.0.1` on an ephemeral port. It is started on demand and stopped when the last sandboxed process with network access exits.
- **Authentication:** `Proxy-Authorization: Basic` with a random 128-bit credential per process. The credential is compared in constant time.
- **Policy checks:** the host is checked against the egress rules for CONNECT and plain HTTP. There is no TLS interception, so the destination is checked by SNI and hostname only.
- **Host syntax (found by the red-team pass, 2026-10-22):** before any rule is matched, the host must be an ASCII host name (non-empty labels of letters, digits, `-` and `_`) or an IP literal. Before this check, a host such as `evil.example%2f.example.com`, or one with U+FF0F FULLWIDTH SOLIDUS before `.example.com`, matched `*.example.com`. A network stack that percent-decodes or IDNA-maps the name would then connect to `evil.example`. The one check (`check_egress_host`) serves tool URLs, redirects and proxy CONNECT requests.
- **Name resolution:** DNS uses the system resolver, and the resolved address is pinned for the connection. Loopback, unspecified, link-local (which includes cloud metadata addresses) and multicast addresses are never connected to, whatever name resolves to them, so an allowed host name cannot be pointed at local services.
- **One destination per connection:** a forwarded plain-HTTP request is sent with `Connection: close`, and proxy headers are removed. A client cannot reuse the connection for another host. `https://` URLs must use CONNECT.
- **Implementation:** one thread per connection with blocking sockets, at most 128 connections; simple, and sufficient for a handful of child processes.
- **Honest limitation:** a process that ignores the proxy variables can connect directly. Only the OS layer stops that. The client reports `proxy-only` unless it detects the endpoint-management firewall profile for Workbench child processes, in which case it reports `os-enforced`. Detection reads the local, Group Policy and MDM firewall rule stores in the registry, looking for an active outbound block rule whose group (`EmbedCtxt`) is `Workbench Child Processes`. It runs at start, and the level is sent with every heartbeat.

### D7. Policy service implementation
- **Stack:** a module of the one control-plane process (Node.js with TypeScript and Express, storage in SQLite through the built-in `node:sqlite` module; decided 2026-10-22). Tables: `policy_groups` (Group definitions and owners), `policy_versions` (one row per draft or published version of a Group or of the org guardrails, with its content as JSON), `policy_scopes` (the version in force, the previous version and the active rollout of each scope), `user_policy_versions` (the latest bundle and per-user version), `dispatch_rules` and `service_signing_keys`. The role mappings and direct assignments that give Groups stay in control-plane/identity; the policy service is told when they change (D13). Audit rows go to the shared audit trail. The process is one node, so compilation, versions and rollout run in one transaction per change and need no cross-node coordination.
- **Validation:** JSON Schema generated from a single schema source. The client's policy parser uses the same source in its tests to prevent drift.
- **Compilation:** the effective policy is compiled on each fetch, from the scope rows and versions of the user's Groups (a few SQLite reads, so no cache is kept). The gateway compiles the same way per request to get `llm.models`, budgets and rate limits. The bundle version is a per-user sequence that increases only when the compiled content changes (D12).
- **Client fixture:** `npm run policy:schema` writes the generated JSON Schema and key list to `client/tests/fixtures/policy/schema.json`; a server test fails when the committed copy differs from the registry.
- **Rollout:** membership is decided by a stable hash of the UPN modulo 100.
- **Alternatives considered:**
  - ASP.NET Core with PostgreSQL (the earlier plan): rejected on 2026-10-22 because the control plane is kept as one Node.js process with SQLite, and PostgreSQL adds a database to operate without a need for it at this scale.
  - Policy files in Git, compiled by a pipeline: no live staged rollout, no per-user version sequence, and no transactional audit of who changed what.

### D8. Example policy (Group "developers")

```yaml
group: developers
priority: 20
llm:
  models: { main: corp-main-large, vision: corp-vision, small: corp-small }
  budget: { period: day, tokens: 3000000 }
  rate_limits: { requests_per_minute: 120, concurrent_requests: 6 }
  run_limits:
    attended:   { model_calls: 60, tool_calls: 200, wall_minutes: 60 }
    unattended: { model_calls: 30, tool_calls: 80,  wall_minutes: 30 }
  max_concurrent_runs: 4
  max_parallel_tools: 4
  max_tool_result_kb: 64
  max_images_per_request: 4
  timeouts: { connect_seconds: 10, idle_seconds: 90 }
tools:
  risk:
    read:        allow
    write-local: allow
    external:    approve
    destructive: approve
  rules:
    "mail.send":  approve      # mail.send also has a tool-declared approval floor
    "computer.*": approve
    "browser.*":  allow
  shell:    { mode: sandboxed, limits: { memory_mb: 2048, wall_minutes: 15, processes: 64, output_kb: 1024 } }
  computer: { mode: allow, app_denylist: [regedit.exe, mmc.exe], retain_screenshots: false }
  browser:  { retain_screenshots: false }
filesystem:
  read:  ["%USERPROFILE%\\Documents\\**", "D:\\DEV\\**"]
  write: ["D:\\DEV\\**", "%WORKBENCH_DATA%\\workspace\\**"]
  deny:  ["**\\.ssh\\**", "**\\*.kdbx", "**\\*.pfx"]
  grantable: ["%USERPROFILE%\\**", "D:\\**"]
network:
  egress_allow: ["*.corp.local", "gitlab.corp.local", "*.nuget.org", "registry.npmjs.org", "pypi.org"]
  egress_deny:  ["*.pastebin.com"]
# MCP servers come from mcp-catalog Group assignments, not from this document
skills:
  install_from: [org, "group:developers"]
  publish: [shared, group, org]   # private is always implicit
knowledge:
  sources: { threads: on, files: on, mail: on, calendar: on, meetings: off }
meeting_capture: { enabled: false }
scheduler: { max_concurrent_runs: 2 }   # Unattended runs; ≤ llm.max_concurrent_runs (cross-key validation)
unattended:
  allowed_risk: [read, write-local]
  rules: { "computer.*": deny }
telemetry: { level: metadata }
storage: { max_size_gb: 10 }
mail: { graph_base_url: "https://graph.microsoft.com/v1.0" }
ui: { background_on_battery: false }
```

Guardrail example (org layer): `network.egress_deny: ["*.consumer-ai-chat.example"]`, `tools.shell.limits.memory_mb ≤ 4096`, and `telemetry.level ≤ metadata` until the organization decides to enable content logging. That approval is organizational and is not enforced technically.

### D9. Schema registry and merge classes
- **One registry.** The schema registry module holds every policy key once, with its type, range, default, owning capability, merge class and guardrail mode. Validation, the merge, the generated JSON Schema (`GET /api/v1/admin/policy/schema`) and the "unknown key" check are all driven by it. A key that no capability owns is rejected on save.
- **Merge classes.** The merge in D2 is made precise by assigning every key one class:
  - *scalar*: the highest-priority Group that sets the key wins; a tie is broken by Group name in ordinal order, so the result never depends on database order.
  - *list union*: entries are unioned; allow lists are then pruned by the deny lists they name (glob match, case-insensitive; `*` matches within a path segment and `**` across segments).
  - *decision map* (`tools.rules`, `tools.risk.*`, `unattended.rules`): per key, deny if any Group denies, otherwise the highest-priority Group.
  - *whole map* (`llm.models`): the highest-priority Group that sets the map decides all of it, so a Group cannot half-inherit another Group's model mapping. A role that the deciding map leaves out is unmapped, which is how a Group removes a role (control-plane/llm-gateway Role Mapping). Only when no Group and no guardrail sets the map does the gateway use its server-side default mapping.
  - *per-key map* (`meeting_capture.notice_text`): per key, the highest-priority Group.
- **Sparse result.** The effective policy contains only the keys that some Group or guardrail sets. The client applies the defaults of the owning capability's spec. The registry defaults are used only for the cross-key rule (D11) and for guardrail comparisons.
- **Boolean and on/off style.** Keys whose owning spec uses `on|off` strings (`knowledge.sources.*`, `knowledge.extraction`, `knowledge.directory_enrichment`) are strings in the registry. All other switches are booleans.

### D10. Guardrail modes
A guardrail document has the shape of a Group document and can only make the effective policy stricter. Each key has a mode, and a key with no meaning for a restriction (for example `filesystem.read`) is rejected in a guardrail.
- *add entries* (deny lists, exclusions, redaction patterns, locked Skills): the guardrail's entries are added.
- *cap* / *floor* (numbers): the effective value is at most or at least the guardrail's. A guardrail applies even when no Group sets the key, if the default of the key would break it.
- *max level* (ordered enumerations such as `tools.shell.mode`, `telemetry.level`): the effective level is not above the guardrail's.
- *restrict* (enumerated lists such as `skills.publish`, `unattended.allowed_risk`): the effective list keeps only the values that the guardrail lists.
- *force* (a single value) and *force entries* (maps such as `llm.models`): the guardrail's value or entries replace the effective ones.
- *stricter* (decision maps): a decision is at least as strict as the guardrail's decision for every key that matches the guardrail's pattern.
Whenever a guardrail changes a Group's value, `src` names the guardrail and simulation lists the conflict with both values. There is one guardrail document for the organization; it applies to everybody at once and has no staged rollout.

### D11. Cross-key rule and publish-time validation
- The rule `scheduler.max_concurrent_runs ≤ llm.max_concurrent_runs` counts the defaults (2 and 3) when a key is unset.
- On save, a document that sets both keys is checked by itself. On publish, the effective policy is compiled for the Group alone and for every combination of Groups that the fleet registry has seen together on an installation, and the publish is refused with `POLICY_CONFLICT` when it creates a rule violation that did not exist before. A combination that no installation has shown can still conflict at bundle time (D12).

### D12. Versions, drafts, rollout, bundles
- **Versions.** A Group (and the guardrail) has at most one draft, which has version 0. Publishing gives it the next number after the highest published version, so version numbers are gapless, and keeps every version for ever. Rollback publishes a new version that copies the previous one.
- **Staged rollout.** A publish with a rollout puts the version in the scope's candidate slot. A *pilot* rollout selects users who are also members of another named Group; a *percent* rollout selects users whose bucket (first four bytes of the SHA-256 of the lower-case UPN, modulo 100) is below the percentage. Selected users compile with the candidate, everybody else with the version in force. Promote makes the candidate the version in force and notifies the users who were not selected; cancel notifies the selected users. While a rollout is active, publish and rollback are refused.
- **Bundle reuse.** Compact JWS with `typ` `workbench-policy+jwt` and the claims of D1, plus `uid` (the token subject) and `schema` (the registry version). A per-user version increases only when the content changes. An unchanged bundle is returned unchanged for the first half of its validity and is then reissued with the same content and a new version.
- **Compile failure.** If a user's combination of Groups cannot be compiled, the service returns the previous bundle while it is still valid (marked `stale`) and otherwise `409 POLICY_COMPILE_FAILED`. Each failure is audited.
- **Keys.** The policy keys are a ring of an ES256 current and next key. A rotated-out key stays published for the bundle validity plus one day.

### D13. Notification
`policy-changed` is sent with `{reason, group?}` through the fleet registry to the installations whose user is in the changed Group (publish, rollback, rollout steps), to the selected or non-selected users of a rollout, and to every installation when a guardrail changes or when the Group mapping changes (the mapping decides each user's Groups). The fleet registry's push targets gained an "everyone" target for this. The policy service is the only sender of this message type.

### D14. Administration, ownership and dispatch rules
- **Roles.** A platform-admin manages everything. A group-owner manages the drafts, versions and rollout of the Groups whose owner list (UPNs) contains them, can read the audit of those Groups, and cannot define Groups, edit guardrails or dispatch rules. A refused attempt on a foreign Group is audited as refused.
- **Dispatch rules** (`/api/v1/admin/policy/dispatch-rules`) are rows of issuer kind (`user`, `role`, `group`, `group-owner`, `service-principal`) and target kind (`user`, `group`, `any`). A request is allowed when some rule matches; with no rule it is denied. control-plane/task-dispatch asks the dispatch-rule service; the rules are never in a bundle.
- **MCP catalog entries** are supplied by control-plane/mcp-catalog through the bundle catalog source and appear in the bundle as `policy.mcp.servers`, with `src` `mcp-catalog`. A Group document that contains `mcp.servers` is rejected. A change of any catalog entry changes the catalog stamp, which is part of the compile cache key.
- **Network enforcement.** The heartbeat may carry `network_enforcement` (`proxy-only` or `os-enforced`). The fleet registry stores it, and simulation shows how many of the user's installations report each level.
## Risks / Trade-offs

- [Proxy bypass by non-proxy-aware tools] → The enforcement level is disclosed. The OS firewall profile is offered for Groups that need hard guarantees. The shell mode is `off` for standard users.
- [A `network: none` script opens sockets directly] → No-network mode refuses everything at the proxy only, so a script that ignores the proxy variables can still connect, like any child process at `proxy-only`. Skills come from the governed registry (client/skills), the refusals are logged, and Groups that need a hard guarantee get the OS firewall profile from IT.
- [Prompt injection steers the model] → The decision never depends on what the model says or what content is in context. The red-team pass (task 5.2, 2026-10-22) checks this in two ways. First, a scripted, fully compromised model obeys injected instructions against the real enforcer. Destructive, undeclared and switched-off tools were denied, and external tools waited for the user. Invented "resolve approval" tools and approval claims had no effect. 20 path escapes and 23 URL escapes were denied, and decisions were identical across untrusted contexts, rationales and arguments. Second, the real model read three injected e-mails and attempted no action beyond reading. The pass found one hole, the egress host syntax (D6), which is now fixed.
- [Path canonicalisation bugs allow escape] → Handle-based final-path checks, the TOCTOU-safe create, fuzzing, and a dedicated escape test corpus (junctions, symlinks, 8.3, ADS, UNC, device paths).
- [Admins write over-permissive Group policies] → Guardrails, simulation with conflict flags, staged rollout, and audit.
- [Policy expiry locks out users who are offline for long periods] → The 7-day default is configurable. Read access to local Threads always works.
- [A stolen policy signing key] → The key lives in the HSM or key vault and is separate from the token key. Rotation is supported by publishing several keys.

## Migration Plan

Not applicable: this is new functionality. Before this change is deployed, the gateway's static role mapping from add-agent-runtime is replaced by `llm.models` from the effective policy.

## Open Questions

- The exact identity-provider role names and the initial Group set (standard, power-users, developers). This is configuration agreed with IT security.
- The name of the endpoint-management firewall rule group used to detect `os-enforced`. This is configuration only.
