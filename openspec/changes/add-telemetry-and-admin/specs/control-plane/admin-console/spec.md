# Spec Delta

## Purpose

Defines the web console that administrators use to operate Skowork: admin roles, the management views over the other control-plane services, the audit viewer and the aggregate usage dashboards. The console exposes functions that other capabilities own and does not redefine them.

## ADDED Requirements

### Requirement: Identity-Provider Authenticated Console
The admin console SHALL sign administrators in at the organization identity provider with OAuth 2.0 Authorization Code and PKCE, and SHALL authorize them with the admin roles of control-plane/identity. It MUST deny access to users who hold none of the admin roles.

#### Scenario: Non-admin user opens console
- **WHEN** an authenticated user who holds no admin role opens the admin console
- **THEN** access is denied and the attempt is audited

### Requirement: Admin Roles
The admin console SHALL define five admin roles. They are assigned to users in the control plane, and control-plane/identity authorizes each request with them:
- platform-admin: all management except individual-level data;
- security-auditor: audit log, individual-level data, re-identification;
- group-owner: policies of owned Groups;
- skill-reviewer: skill review queue;
- works-council-reviewer: read-only records of individual-level access.

Every action MUST be authorized against these roles.

#### Scenario: Group owner edits another group
- **WHEN** a group-owner tries to edit the policy of a Group they do not own
- **THEN** the console refuses the action

#### Scenario: Skill reviewer scope
- **WHEN** a skill-reviewer opens the console
- **THEN** only the skill review queue and read-only skill catalog views are available

#### Scenario: Works council reviewer scope
- **WHEN** a works-council-reviewer opens the console
- **THEN** only the records of individual-level data access are shown, read-only, and no telemetry or policy view is available

### Requirement: Role-Scoped Views And Refusal Audit
The admin console SHALL show each admin only the views that their admin roles allow, and the control plane SHALL check the same roles on each view's API. Every action that the control plane refuses for lack of a role MUST be audited.

#### Scenario: Refused action audited
- **WHEN** an admin calls an admin API that their roles do not allow
- **THEN** the call is refused, and an audit event records the admin, the method and the path

### Requirement: Policy Administration
The admin console SHALL expose the administration functions of control-plane/policy-service to authorized admins. Those functions are Group role mappings and direct assignments, policy documents and priorities, guardrails, validation, the Policy Diff, simulation, staged rollout and Remote task dispatch rules. Their behaviour is defined in control-plane/policy-service, which the console does not redefine.

#### Scenario: Validation error shown
- **WHEN** control-plane/policy-service rejects a policy document that an admin saves
- **THEN** the console shows the returned validation errors next to the affected fields, and nothing is published

#### Scenario: Simulation shown with sources
- **WHEN** an admin simulates the effective policy for a user
- **THEN** the console shows the merged result returned by control-plane/policy-service, with the source Group or guardrail of each setting

#### Scenario: Staged rollout status
- **WHEN** an admin publishes a policy version to a pilot Group or a percentage, as control-plane/policy-service supports
- **THEN** the console shows adoption by policy version

### Requirement: Model And Quota Administration
The admin console SHALL let platform-admins set, for each Group, the model role mappings in policy.llm.models, which also act as the model allowlist. It also covers the token budgets in policy.llm.budget and the rate limits in policy.llm.rate_limits. These keys are owned by control-plane/llm-gateway and are published through control-plane/policy-service.

#### Scenario: Change model mapping
- **WHEN** a platform-admin maps the main model role of a Group to a different model and publishes
- **THEN** new Runs of that Group's users use the newly mapped model

### Requirement: Identity Administration
The admin console SHALL let platform-admins manage the identity functions of control-plane/identity: registering and retiring service principals together with the APIs each one may call, and maintaining the deny list of users and installations.

#### Scenario: Register a service principal
- **WHEN** a platform-admin registers the CI system as a service principal allowed to call task dispatch
- **THEN** control-plane/identity issues service tokens to it for that API only, and the registration is audited

#### Scenario: Deny a user
- **WHEN** a platform-admin adds a user to the deny list
- **THEN** control-plane/identity refuses that user's next token request

### Requirement: Remote Task Administration
The admin console SHALL let platform-admins manage the Remote task dispatch rules of control-plane/policy-service, and the per-issuer rate limits of control-plane/task-dispatch.

#### Scenario: Lower an issuer's rate limit
- **WHEN** a platform-admin lowers the rate limit of a registered system to 20 tasks per hour
- **THEN** control-plane/task-dispatch applies the new limit to that issuer's subsequent requests

### Requirement: MCP Catalog Administration
The admin console SHALL let platform-admins add, update, disable and assign MCP server entries in control-plane/mcp-catalog to Groups.

#### Scenario: Disable MCP server
- **WHEN** a platform-admin disables an MCP server entry
- **THEN** the entry is no longer offered to any Group, and clients receive the change through their Policy bundle

### Requirement: Skill Review Queue
The admin console SHALL show skill-reviewers the queue of Skill versions submitted for org scope in control-plane/skill-registry. Each queue item shows the automated scan results and a diff against the previous version, and lets the reviewer approve or reject it with a comment. Platform-admins can also start a public Skill import from the console.

#### Scenario: Approve skill
- **WHEN** a skill-reviewer approves a submitted Skill version
- **THEN** control-plane/skill-registry signs and publishes it, as that capability defines

#### Scenario: Import public skill
- **WHEN** a platform-admin imports a public Skill from its source URL
- **THEN** the Skill enters the review queue with its source and licence shown

### Requirement: Fleet Administration
The admin console SHALL show the installations recorded by control-plane/fleet-registry, filterable by version, policy version, online state and the user's Groups recorded by the fleet registry. It also exposes the registry's functions to revoke an installation or a user and to set the minimum supported version for each channel.

#### Scenario: Revoke from console
- **WHEN** a platform-admin revokes an installation from the fleet view
- **THEN** control-plane/fleet-registry performs the revocation, and the console shows the installation as revoked

### Requirement: Audit Log Viewer
The admin console SHALL record every admin action as an audit event, alongside the authentication and security events of the other control-plane services. Security-auditors can search, filter and export these events. Audit events MUST be append-only.

#### Scenario: Audit of policy publish
- **WHEN** an admin publishes a policy version
- **THEN** an audit event records the admin, the Group, the old and new versions, and the time

#### Scenario: Audit event cannot be changed
- **WHEN** any process tries to update or delete a stored audit event
- **THEN** the audit store refuses the change and the event stays as it was

### Requirement: Aggregate Usage Dashboards
The admin console SHALL provide usage dashboards aggregated over Groups: active installations, Runs, tool usage, token consumption against quotas, error rates and version adoption. The dashboards MUST NOT rank, compare or display individual users' activity.

#### Scenario: Dashboard aggregation floor
- **WHEN** an admin views usage for a Group with fewer than 5 active users in the selected period
- **THEN** the dashboard suppresses that Group's figures to prevent identifying individuals

### Requirement: Individual-Level Data Access
Only the security-auditor role SHALL access telemetry or audit data about an individual user. That covers a user's telemetry records and crash dumps, the re-identification of a pseudonym, and audit events searched by user. The auditor MUST state a reason for each access. Each access is recorded, and works-council-reviewers can review those records.

#### Scenario: Auditor investigates a user
- **WHEN** a security-auditor opens individual-level telemetry for a user
- **THEN** the console asks for a reason and records the access with that reason before showing data

#### Scenario: Access without a reason
- **WHEN** a security-auditor requests individual-level data without stating a reason
- **THEN** the request is refused and no data is shown

#### Scenario: Platform admin denied
- **WHEN** a platform-admin tries to open individual-level telemetry
- **THEN** access is denied

#### Scenario: Works council review
- **WHEN** a works-council-reviewer opens the access records
- **THEN** each individual-level access is listed with auditor, user, time and reason
