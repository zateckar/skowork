# Proposal

## Why

To run a managed MVP pilot, operators need two things. First, privacy-preserving operational telemetry, so they can see whether clients are healthy, how they are used and where they fail. Second, a single admin console for groups, policies, models, MCP servers, skills and the fleet. Without both, the control plane cannot be operated, and the organization-level approval process cannot be shown exactly what is collected.

## What Changes

- Add client telemetry based on OpenTelemetry:
  - three levels: off, metadata (the default) and content, with a persistent user indicator at content level;
  - an encrypted offline buffer with a size cap;
  - crash reports;
  - an in-app Telemetry viewer that shows the user exactly what was sent.
- Add the control-plane telemetry ingest: it authenticates OTLP over HTTPS with Workbench tokens, optionally pseudonymizes user identifiers, applies retention, and forwards to the organization observability/SIEM stack.
- Add the admin console:
  - a web UI signed in through the organization identity provider with five admin roles: platform-admin, security-auditor, group-owner, skill-reviewer and works-council-reviewer;
  - views over the functions owned by other capabilities:
    - policy administration (control-plane/policy-service);
    - model roles, budgets and rate limits (control-plane/llm-gateway keys);
    - service principals (control-plane/identity, added by add-scheduled-and-remote-tasks) and the deny list (control-plane/identity);
    - Remote task dispatch rules and issuer rate limits;
    - the MCP catalog;
    - the skill review queue and public import;
    - the fleet;
    - the audit log;
  - aggregate usage dashboards.
- The design suits the works council: there are no per-user productivity views. Only the security-auditor can access individual-level data, every access is recorded, and the works-council-reviewer can review those records.

Out of scope:
- Approval to use content-level telemetry in production. This is an organizational prerequisite handled at organization level, with the MS Teams recording policy as precedent. It is not a technical mechanism. Development and limited testing may use any level.
- The behaviour of policy compilation and signing (control-plane/policy-service), model routing (control-plane/llm-gateway), MCP catalog semantics (control-plane/mcp-catalog) and skill review semantics (control-plane/skill-registry). The console only exposes these.

## Capabilities

### New Capabilities
- `client/telemetry`: what the client collects at each telemetry level, buffering, crash reports, and the user-facing Telemetry viewer
- `control-plane/telemetry-ingest`: authenticated OTLP ingest, pseudonymization, retention and forwarding to organization observability
- `control-plane/admin-console`: admin web UI, admin roles, management views over the other control-plane services, audit viewer and aggregate dashboards

### Modified Capabilities
- `control-plane/policy-service`: reports rollout adoption by policy version, which the console's policy view shows.

## Impact

- **Roadmap:** change 7 of 13 (MVP). It depends on:
  - add-client-foundation: identity, fleet registry, local storage;
  - add-agent-runtime: the Run and tool events that telemetry describes;
  - add-policy-and-sandbox: policy-service, which the console edits (including its Policy Diff), and the policy decision records and network enforcement level that telemetry reports;
  - add-work-inbox: the Privacy settings section that hosts the Telemetry viewer.
- **Relies on control-plane/fleet-registry** (add-client-foundation) recording each installation user's Groups, for the fleet view filter.
- **Forward references:** the console's MCP, skill and Remote task views reach their full scope only when later changes are in place:
  - control-plane/mcp-catalog (add-core-tools-and-mcp, change 5, earlier);
  - control-plane/skill-registry (add-skills, change 9);
  - control-plane/task-dispatch (add-scheduled-and-remote-tasks, change 10);
  - service principals, which the identity management view shows and which add-scheduled-and-remote-tasks defines (change 10).

  Until then those views are empty placeholders. The changes that add those services also build the console views over them (add-skills: the review queue and public import; add-scheduled-and-remote-tasks: service principals, issuer rate limits and the callback allowlist).
- **Policy keys owned by client/telemetry:**
  - `policy.telemetry.level` (off | metadata | content, default metadata)
  - `policy.telemetry.buffer_max_mb` (default 20)
  - `policy.telemetry.crash_reports` (default true)
  - `policy.telemetry.redaction_patterns` (list of regular expressions added to the built-in patterns, default empty)
- **Content level** means `policy.telemetry.level` is content. The telemetry ingest drops content from senders that are not at that level.
- **New code:**
  - `client/` telemetry module;
  - `control-plane/` telemetry-ingest and admin-console modules of the control-plane process;
  - an admin single-page app in React and TypeScript, served by the control-plane process.
- **Infrastructure:**
  - an OTLP forwarding target in the organization observability or SIEM stack;
  - admin role assignments for the five admin roles, made in the control plane.
- **Governance (organizational, not technical):** before production rollout, the organization-level approval process (data protection officer, works council) receives the metadata catalogue (design.md D2). Development and limited testing do not wait for it.
