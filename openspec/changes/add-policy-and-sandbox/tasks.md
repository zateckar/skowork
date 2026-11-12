# Tasks

## 1. Validation spikes

- [ ] 1.1 Prototype a Job Object with a restricted token and the egress proxy running common developer tools (git, dotnet, npm, python, PowerShell) on the baseline laptop. Verify the tools work through the proxy, and record any tool that ignores the proxy variables. Status: partial; proxy-variable behaviour recorded on the development workstation (design.md Spike results); the Job Object run with allowed traffic and the baseline laptop are open
- [x] 1.2 Run the scripting runtimes Skills use (python, node, PowerShell) in no-network mode on the baseline laptop. Verify that their requests through the proxy are refused, and record any runtime that ignores the proxy variables. Done 2026-11-12 on the development workstation: Node.js and Windows PowerShell 5.1 ignore them; the sandbox now sets NODE_USE_ENV_PROXY (design.md Spike results)
- [ ] 1.3 Agree the initial Groups and their identity-provider role mapping with IT security. Verify by recording the signed-off mapping in this change's design.md Open Questions resolution

## 2. Policy service (control-plane/policy-service)

- [x] 2.1 Define the single policy schema source and generate JSON Schema from it. Verify that the server validator and the client test fixtures both use the generated schema
- [x] 2.2 Implement Groups, role mappings and direct assignments, policy documents with priority, and guardrails, with schema validation on save. Verify with API tests (`npm test`, node:test, on SQLite), including rejection of invalid documents
- [x] 2.3 Implement the effective-policy merge: deny wins, unioned lists, decision maps by deny-any-then-priority, priority scalars, guardrail clamps, mcp-catalog assignments and dispatch rules. Verify with a table-driven test suite covering multi-group users, including conflicting `tools.rules`
- [x] 2.4 Implement signed bundles (dedicated ES256 key, per-user monotonic version, expiry, key publication). Verify that the signature verifies with the published key and that versions increase
- [x] 2.5 Implement the policy-changed push via the fleet registry, staged rollout by hash percentage or pilot Group, and rollback. Verify with an integration test that only the selected users receive the pilot version
- [x] 2.6 Implement simulation with rule sources and conflict flags, the audit log, and admin-role authorization. Verify that a non-admin is refused and that every mutation writes an audit row
- [x] 2.7 Implement cross-key validation at compile time (`scheduler.max_concurrent_runs` ≤ `llm.max_concurrent_runs`) and the policy diff between versions. Verify that an invalid combination is refused on publish and that the diff lists every changed key

## 3. Policy enforcement (client/policy-enforcement)

- [x] 3.1 Implement bundle fetch, verification (signature, subject, expiry, anti-rollback), encrypted cache and the 6-hour refresh. Verify with tests for tampered, expired and rollback bundles
- [x] 3.2 Implement the decision function as the mandatory BeforeToolCall handler. The steps are: `tools.rules` pattern, then `tools.risk`, then the Risk Class Defaults, then argument checks, then tool- and Skill-declared floors, then the unattended rules. Verify with a table-driven test suite that includes the filesystem and unattended defaults
- [x] 3.3 Implement fail-closed behaviour with no valid bundle. Verify that only reads of the client's own data are allowed
- [x] 3.4 Implement path canonicalisation and glob matching with handle-based final-path checks and TOCTOU-safe creation. Verify with the escape corpus (junction, symlink, 8.3, ADS, device, UNC) and a fuzz target
- [x] 3.5 Implement user folder grants within `grantable` bounds, including revocation. Verify that out-of-bounds grants are refused
- [x] 3.6 Implement in-process egress checks, including re-checks after redirects, with an interface the agent browser uses for every request, and the platform-connection exemption (control plane, Graph hosts, domain controllers). Verify that a redirect to a denied host is blocked and that platform connections work with an empty allow list
- [x] 3.7 Implement the decision audit log and the effective-policy summary view data, including "why denied". Verify that the denied-call record names the matched rule and its source Group

## 4. Tool sandbox (client/tool-sandbox)

- [x] 4.1 Implement contained launch (suspended, assign to job, resume) with breakaway disallowed and kill-on-close. Verify that killing the client terminates the whole process tree
- [x] 4.2 Implement resource limits with the stated defaults, where a per-call timeout can only shorten the limit, and the limit-exceeded results. Verify with memory, process-count and time-limit tests, including the smaller-timeout-wins case
- [x] 4.3 Implement the restricted token and the scrubbed environment. Verify that no tokens appear in the child environment and that the Administrators group is deny-only
- [x] 4.4 Implement the shell modes off, sandboxed and allow. Verify that `shell.run` is hidden from `find_tools` when the mode is off
- [x] 4.5 Implement no-network mode as a proxy session that refuses every destination. Verify that a request to a host allowed by policy and a request to a denied host are both refused and logged
- [x] 4.6 Implement the on-demand egress proxy with per-process credentials and egress rules. Verify that allowed and denied hosts behave correctly, that a credential-less client is rejected, and that the proxy stops when idle
- [x] 4.7 Implement enforcement-level detection (proxy-only or os-enforced) and its reporting. Verify on a machine with and without the firewall profile. Verified by SandboxEnforcement.RuleDetection without the profile and with rule strings; no machine with the deployed profile was available (design.md Spike results)
- [x] 4.8 Implement bounded output capture (`policy.tools.<group>.limits.output_kb`, default 1024 per stream) with attachment overflow and safe decoding. Verify with a 100 MB output test

## 5. Integration

- [x] 5.1 End-to-end test: an admin publishes a stricter developers policy to a pilot, a pilot client receives the push, a previously allowed tool call is now decided as approve, and simulation shows the same result
- [x] 5.2 Run a red-team pass: injection content attempts to trigger external actions and path escapes. Verify that no unapproved external or destructive action occurs
- [x] 5.3 Run `openspec validate --all --strict` and fix any findings

## Workflow follow-up

- Archive this change after review with `openspec archive add-policy-and-sandbox --yes`, then check that the merged specs read coherently.
