# Proposal

## Why

Users repeat the same kinds of work, such as weekly reports, change-request reviews and supplier email triage. They should be able to capture how they do this work once, as a skill, and share it with colleagues. Sharing skills is how know-how spreads across the organization. Because skills can carry scripts and tool permissions, sharing beyond a small circle needs review, signing and the ability to revoke a skill centrally.

## What Changes

- **Skill format:** add `SKILL.md` packages, compatible with agentskills.io and Claude. Frontmatter fields:
  - `name`, `description`, `version`
  - `tools`, `risk`, `network`, `requires_mcp`
  - `interrupt_on`, which can only make decisions stricter
  - optional `triggers`, which are suggested Triggers and are created only after the user confirms

  Optional folders are `scripts/` (PowerShell), `references/` and `assets/`.
- **Loading:** skills load from five tiers: built-in, org, group, shared and private. Precedence and name conflicts are defined.
- **Running:** each skill runs as a tool-scoped subagent. The agent selects a skill automatically from its description, or the user invokes it with `/name` in the composer.
- **Authoring:** an in-app editor; "Save this Thread as a skill", where the agent drafts the skill and the user edits it; and a local test run.
- **Sharing scopes:** private, shared (named people), group and org. The org scope requires:
  - an automated scan;
  - approval by a skill-reviewer;
  - signing.
- **Installation:** installed versions are pinned. Policy controls automatic updates, and rollback is available. A revocation is pushed (skill-revoked) and is also delivered at the next Skill sync. Syncs run at start, after reconnecting, on a skills-changed push, and at least every 24 hours.
- **MCP gating:** a skill that requires MCP servers installs only if policy allows those servers.
- **Usage counts:** authors see metadata-only usage counts.
- **Public skills:** they can be imported only through org review.

Out of scope:
- A public marketplace.
- Script runtimes other than PowerShell, unless policy provides one.
- Paid or licensed skills.

## Capabilities

### New Capabilities
- `client/skills`: skill format, loading tiers, selection and invocation, authoring, testing, installation, updates and revocation on the client
- `control-plane/skill-registry`: storage, versioning, sharing scopes, review and signing, distribution, revocation and usage counts

### Modified Capabilities
- None.

## Impact

- **Roadmap:** change 9 of 13 (Managed).
- **Depends on:**
  - add-client-foundation: fleet push with the skills-changed and skill-revoked types, and local storage
  - add-agent-runtime: subagents, tool scoping and resolutions
  - add-policy-and-sandbox: the skill-declared decision floor, tool-sandbox no-network mode and egress proxy, policy keys
  - add-work-inbox: composer `/name`, editor UI
  - add-core-tools-and-mcp: shell.run for skill scripts, MCP availability checks
  - add-telemetry-and-admin: the review queue UI
- **Forward reference:** the `triggers` frontmatter offers Triggers through client/scheduler, which add-scheduled-and-remote-tasks (change 10) introduces. Until then, suggested Triggers are shown but cannot be created.
- **New code:**
  - `client/` skills module and editor UI;
  - `control-plane/` Skill Registry service, backed by the internal GitLab.
- **Policy keys owned by client/skills:**
  - `policy.skills.install_from` (default [org, group, shared])
  - `policy.skills.publish` (default [shared])
  - `policy.skills.auto_update` (default true)
  - `policy.skills.script_runtimes` (default [powershell])
  - `policy.skills.locked` (default empty)
- **Infrastructure:**
  - an internal GitLab group for skill repositories;
  - a signing key held by the registry in the control-plane secret store.
