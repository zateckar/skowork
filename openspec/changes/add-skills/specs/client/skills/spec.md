# Spec Delta

## Purpose

Defines how the client loads, selects, runs, authors, tests, installs, updates and removes Skills. A Skill is a reusable SKILL.md package that captures a way of doing a task.

## ADDED Requirements

### Requirement: Skill Package Format
The client SHALL accept Skills that are a folder containing a SKILL.md file. The file has YAML frontmatter, and its markdown body is the Skill's instructions. These frontmatter fields are required:
- name: lowercase kebab-case
- description
- version: semantic

These fields are optional: tools, risk, network, requires_mcp, interrupt_on and triggers. The optional folders scripts/, references/ and assets/ are allowed. Unknown fields are preserved and ignored.

#### Scenario: Valid skill loads
- **WHEN** a Skill folder contains a SKILL.md with a valid name, description and version
- **THEN** the client loads the Skill and lists it in the Skills view

#### Scenario: Invalid frontmatter
- **WHEN** a SKILL.md lacks a description or has a name that is not kebab-case
- **THEN** the client does not load the Skill and shows the validation error in the Skills view

### Requirement: Agent Skills Compatibility
The client SHALL load SKILL.md packages that follow the agentskills.io format, which needs only name and description. When version is missing, the client treats it as 0.0.0.

#### Scenario: Minimal external skill
- **WHEN** a private Skill has only name and description in its frontmatter
- **THEN** it loads with version 0.0.0 and with the default Skill tool set

### Requirement: Skill Tool Scope
When a Skill runs, the client SHALL limit its tools to the intersection of the Skill's declared tools and the tools the user's policy allows. A Skill that declares no tools gets the default Skill tool set: the read-class tools the user's policy allows. A Skill MUST NOT gain any tool or approval exemption that policy does not already grant.

#### Scenario: Skill declares a disallowed tool
- **WHEN** a Skill declares shell.run but the user's policy sets policy.tools.shell.mode to off
- **THEN** the Skill runs without shell.run and the Skills view shows that tool as unavailable

#### Scenario: Skill declares no tools
- **WHEN** a Skill without a tools field runs
- **THEN** it can use only the read-class tools that the user's policy allows

### Requirement: Skill Interrupt Rules
The client SHALL apply a Skill's interrupt_on rules as a skill-declared decision floor under client/policy-enforcement. That floor can only make decisions stricter. A rule can require approval for a tool, and it can restrict which client/agent-runtime resolutions are accepted for it. A rule MUST NOT loosen any decision.

#### Scenario: Skill adds approval
- **WHEN** a Skill declares interrupt_on for mail.draft with the resolutions approve and reject
- **THEN** each mail.draft call in that Skill is decided at least as approve, and only the approve and reject resolutions are accepted

#### Scenario: Skill tries to remove an approval
- **WHEN** a Skill's interrupt_on marks a tool as not needing approval, but policy decides that tool as approve
- **THEN** the tool is still decided as approve

### Requirement: Skill Risk Declaration
The client SHALL treat a Skill's risk field as the highest Risk class the Skill intends to use; the default is read. The declared risk is shown in the Skills view and during registry review. A call above the declared risk is decided at least as approve, through the skill-declared decision floor of client/policy-enforcement.

#### Scenario: Call above declared risk
- **WHEN** a Skill that declares risk write-local calls an external tool that policy allows
- **THEN** that call is decided as approve

### Requirement: Skill Network Declaration
The client SHALL run a Skill's scripts in the no-network mode of client/tool-sandbox when the Skill's network field is none. When the field is proxy, which is the default, the scripts use the egress proxy under policy.network rules.

#### Scenario: Script without network
- **WHEN** a Skill that declares network none runs a script that tries to download a file
- **THEN** the connection fails, because the script runs without network access

### Requirement: Suggested Triggers
The client SHALL treat a Skill's triggers field as suggested Trigger definitions. When the Skill is installed or first used, the client offers to create them through client/scheduler. The client MUST NOT create a Trigger from a Skill without the user's confirmation.

#### Scenario: Skill suggests a weekly schedule
- **WHEN** a user installs a Skill whose triggers field suggests a Monday 08:00 schedule
- **THEN** the client offers to create that Trigger, and no Trigger exists until the user accepts

### Requirement: Skill Load Tiers
The client SHALL load Skills from five tiers, from lowest to highest precedence: built-in, org, group, shared, private. When two tiers contain a Skill with the same name, the client SHALL use the higher-precedence one and show the shadowing in the Skills view. Org Skills named in policy.skills.locked (default empty) cannot be shadowed.

#### Scenario: Private shadows org
- **WHEN** a user has a private Skill with the same name as an org Skill
- **THEN** the private Skill is used and the Skills view marks the org Skill as shadowed

#### Scenario: Locked org skill
- **WHEN** an org Skill is listed in policy.skills.locked and the user creates a private Skill with the same name
- **THEN** the org Skill remains in use and the private Skill is flagged as conflicting

### Requirement: Skills Run As Subagents
The client SHALL run a selected Skill as a tool-scoped subagent of the current Run, as defined by client/agent-runtime. The Skill's instructions form the subagent's system context, and its progress appears in the Thread's Activity log.

#### Scenario: Skill invoked during a Run
- **WHEN** the agent selects the Skill "weekly-report" during a Run
- **THEN** a subagent starts with that Skill's instructions and tool scope, and the Activity log shows it as a delegation

### Requirement: Automatic Skill Selection
The client SHALL offer the names and descriptions of all available Skills to the main agent. This lets the agent select a Skill when the user's request matches the Skill's description.

#### Scenario: Matching request
- **WHEN** the user asks for this week's status report and an available Skill describes weekly status reports
- **THEN** the agent may delegate to that Skill without the user naming it

### Requirement: Explicit Skill Invocation
The client SHALL let the user invoke a Skill explicitly by typing /name in the composer, with completion over the available Skills. An explicitly invoked Skill MUST be used for that Turn.

#### Scenario: Slash invocation
- **WHEN** the user sends "/supplier-triage last 20 emails"
- **THEN** the Run starts the Skill supplier-triage with the remaining text as its input

### Requirement: Skill Scripts
The client SHALL execute a Skill's scripts only through the shell.run tool of client/core-tools, under client/tool-sandbox, and only with runtimes listed in policy.skills.script_runtimes. Values are runtime names (powershell, plus any runtime that policy provides). The default is [powershell].

#### Scenario: Python script without runtime
- **WHEN** a Skill calls a Python script and policy.skills.script_runtimes does not include Python
- **THEN** the call fails with an error naming the missing runtime, and nothing is executed

### Requirement: Skill Authoring
The client SHALL provide a Skill editor. The editor lets the user create, edit and delete private Skills, edit the frontmatter and body with validation, and manage the files in the Skill's folders.

#### Scenario: Create private skill
- **WHEN** the user creates a new Skill in the editor and saves it with valid frontmatter
- **THEN** it is stored in the private tier and is immediately available to new Runs

### Requirement: Save Thread As Skill
The client SHALL offer "Save as skill" on a Thread. The agent drafts a SKILL.md from the Thread, generalising the steps, tools and inputs used, and the user reviews and edits the draft before saving. A draft MUST NOT include personal data from the Thread unless the user keeps it in.

#### Scenario: Draft from Thread
- **WHEN** the user chooses "Save as skill" on a completed Thread
- **THEN** the editor opens with a drafted name, description, tool list and instructions derived from that Thread

### Requirement: Skill Test Run
The client SHALL let the user test a Skill against a sample input in a separate test Thread before saving or publishing it. The test run applies the same tool scope and approvals as a normal run.

#### Scenario: Test before publish
- **WHEN** the user runs a test of a draft Skill
- **THEN** a test Thread is created, the run uses the draft's tool scope, and the Thread is labelled as a test

### Requirement: Skill Publishing
The client SHALL let the user publish a Skill to the scopes in policy.skills.publish. Values are a list drawn from shared, group and org, and the default is [shared]. The private scope is always available. Publishing to org submits the Skill for review in control-plane/skill-registry, and it is not available until approved.

#### Scenario: Share with colleagues
- **WHEN** the user shares a private Skill with two named colleagues
- **THEN** those colleagues receive it in their shared tier on their next Skill sync

#### Scenario: Publish to org pending review
- **WHEN** the user publishes a Skill to the org scope
- **THEN** the Skill shows "In review" and is not installed for others until a skill-reviewer approves it

### Requirement: Skill Installation And Pinning
The client SHALL install shared, group and org Skills from the skill registry at pinned versions, and only from the sources in policy.skills.install_from. Values are a list of org, group (all of the user's Groups), group:<name> and shared. The default is [org, group, shared]. The client MUST verify the registry signature of org Skills before loading them.

#### Scenario: Signature mismatch
- **WHEN** an org Skill's content does not match its registry signature
- **THEN** the client refuses to load it and records a security event

### Requirement: Skill Updates And Rollback
The client SHALL update installed Skills automatically when policy.skills.auto_update is true, which is the default. Otherwise it SHALL offer updates to the user. The user can roll back any installed Skill to its previously installed version.

#### Scenario: Rollback
- **WHEN** a user rolls back an updated Skill
- **THEN** the previous version is restored and pinned until the user chooses to update again

### Requirement: Skill MCP Requirements
The client SHALL install or enable a Skill that declares requires_mcp only when every listed MCP server is allowed by the user's policy. Otherwise the Skill is shown as unavailable, with the missing servers named.

#### Scenario: Missing MCP server
- **WHEN** a group Skill requires the MCP server jira-corp and the user's policy does not allow it
- **THEN** the Skill is listed as unavailable with "requires jira-corp"

### Requirement: Skill Sync
The client SHALL sync its Skill list with control-plane/skill-registry at start, after reconnecting, on a skills-changed push, and at least every 24 hours.

#### Scenario: Skill published while offline
- **WHEN** a group Skill is published while the client is offline
- **THEN** the client receives it in the sync after reconnecting

### Requirement: Skill Revocation
The client SHALL remove a revoked Skill when it receives a skill-revoked push through control-plane/fleet-registry. If it missed the push, it SHALL remove the Skill at the next Skill sync. A Run using a revoked Skill MUST stop that Skill's subagent.

#### Scenario: Revoked during a Run
- **WHEN** a revocation arrives for a Skill whose subagent is running
- **THEN** the subagent is stopped, the Thread shows that the Skill was revoked, and the Skill disappears from the Skills view

#### Scenario: Revoked while offline
- **WHEN** a Skill is revoked while the client is offline for longer than push retention
- **THEN** the client removes the Skill during the Skill sync after reconnecting
