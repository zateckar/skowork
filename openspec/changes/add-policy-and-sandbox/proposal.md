# Proposal

## Why

Different user groups (standard users, power users, developers) need different LLM access, tools, file and network restrictions, and these must be managed centrally. Every tool call must be governed by those rules, so that a model, or injected content, cannot act beyond what the user's Group permits. This change adds central policy authoring and distribution, client-side enforcement, and process isolation for anything the agent executes.

## What Changes

- Add the control-plane policy service:
  - map identity-provider roles and direct assignments to Skowork Groups (control-plane/identity);
  - hold policy documents per Group with priorities and an org guardrail layer;
  - compile the effective policy per user;
  - sign, version and expire policy bundles;
  - support staged rollout, simulation, diff, schema and cross-key validation, and audit;
  - push policy-changed notifications through the fleet registry.
- Add client policy enforcement:
  - verify and cache policy bundles;
  - make an allow, approve or deny decision for every tool call by Risk class and tool id, with separate rules for Unattended runs;
  - enforce filesystem path rules (with escape checks) and user folder grants within policy bounds;
  - enforce network egress rules for web.fetch, remote MCP, the agent browser and sandboxed child processes, with defined exemptions for platform connections;
  - guarantee that untrusted content never approves or escalates;
  - fail closed when no valid policy is available.
- Add the client tool sandbox:
  - child processes (shell, skill scripts, stdio MCP servers, workers) run in Job Objects with resource limits and a restricted token;
  - shell modes off, sandboxed and allow;
  - a no-network mode;
  - a loopback egress proxy with per-process credentials that applies the network policy to child processes.

Out of scope:
- The admin UI for editing policies (control-plane/admin-console, change 7). This change provides the API.
- OS-level firewall or WFP rules. These are a third enforcement layer, deployed through endpoint management and documented in design.md.
- How approvals are presented (client/work-inbox, change 4).

## Capabilities

### New Capabilities
- `client/policy-enforcement`: bundle verification and caching, tool-call decisions, attended and unattended rules, filesystem and network rules, user grants, fail-closed behaviour
- `client/tool-sandbox`: process isolation, resource limits, shell modes, no-network mode, egress proxy for child processes
- `control-plane/policy-service`: Group mapping, policy documents, guardrails, merge into the effective policy, signing, versioning, rollout, simulation, audit

### Modified Capabilities
- None.

## Impact

- **Roadmap:** change 3 of 13 (MVP).
  - It depends on change 1, add-client-foundation (identity, control channel, fleet registry push) and change 2, add-agent-runtime (the mandatory BeforeToolCall hook, Risk classes, interrupts).
  - Every tool change (5 onwards) relies on its decisions, path rules and sandbox.
- **Forward references** to capabilities that later changes introduce:
  - control-plane/mcp-catalog (change 5): catalog entries included in the bundle;
  - control-plane/admin-console (change 7): administrator roles for the administration API;
  - client/skills (change 9): the Skill-declared floor and `network: none`;
  - control-plane/task-dispatch (change 10): dispatch rules;
  - client/mail-calendar (change 11) and client/computer-use (change 8): examples of tool-declared floors, and the Graph platform connection;
  - client/mcp-client (change 5): stdio MCP servers contained by the tool sandbox;
  - client/telemetry (change 7): reporting of decision audit records and the network enforcement level;
  - client/knowledge-graph (change 12): the directory platform connection.
  - Until those land, the related bundle sections are empty.
- **Policy keys owned here:**
  - `policy.tools.rules`, `policy.tools.risk`, `policy.unattended.*`, `policy.filesystem.*`, `policy.network.egress_allow` and `egress_deny` (client/policy-enforcement);
  - `policy.tools.shell.mode` and `policy.tools.<group>.limits` (client/tool-sandbox).
  - Keys under other tool groups are owned by their tool capabilities.
- **New code:**
  - `client/policy` and `client/sandbox`, plus the egress proxy
  - `control-plane/policy-service` module of the control-plane process, with SQLite tables for groups, policies, versions and audit
- **Infrastructure:**
  - a policy signing key in the control-plane key store (HSM if available);
  - the identity-provider role mappings to Groups, agreed with IT security (control-plane/identity);
  - optional endpoint-management firewall profiles for hard network enforcement.
- **Security review:** the merge semantics, path canonicalisation, the sandbox escape surface, and proxy authentication.
