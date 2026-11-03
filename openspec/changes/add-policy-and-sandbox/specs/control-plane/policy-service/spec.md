# Spec Delta

## Purpose

Defines how the control plane manages Groups and their policies, merges them into one effective Policy bundle per user, signs and distributes it, and lets administrators roll out, simulate and audit policy changes.

## ADDED Requirements

### Requirement: Group Mapping
The policy service SHALL let administrators map identity-provider roles to Groups, and assign users to Groups directly. Group membership is determined by control-plane/identity, which takes it from these mappings and assignments. Regional differences, for example a country where a feature is rolled out, are expressed as Groups. There is no separate region concept.

#### Scenario: Regional Group
- **WHEN** an administrator maps the identity-provider role of Czech employees to the Group "region-eu" and enables meeting capture only in that Group's policy
- **THEN** only members of "region-eu" receive a bundle with meeting capture enabled

#### Scenario: Map role
- **WHEN** an administrator maps the role "workbench-developer" to the Group "developers"
- **THEN** users whose token carries that role are members of "developers" on their next request

### Requirement: Group Definition And Ownership
The policy service SHALL keep a definition for each Group: its name, a description and its owners, who are named user principals. A Group is defined by an administrator, or implicitly when the first policy document is saved for it. A user who holds the group-owner role and is an owner of a Group MAY manage that Group's policy documents, versions and rollout, and MUST NOT manage any other Group, the guardrails, the Group mappings or the schema.

#### Scenario: Group owner edits own Group
- **WHEN** a group-owner who is an owner of "developers" saves a draft for "developers"
- **THEN** the draft is saved and the change is audited

#### Scenario: Group owner edits another Group
- **WHEN** the same group-owner saves a draft for "finance"
- **THEN** the request is refused and audited

### Requirement: Policy Documents Per Group
The policy service SHALL store policy documents per Group, each with a priority, which is an integer where a higher value wins. It SHALL validate every document against the policy schema before saving, so that a document with unknown top-level keys or invalid values is rejected. The top-level keys are those of the Policy bundle glossary entry. The mcp-catalog servers are not a key of a policy document, so a document that contains policy.mcp.servers is rejected.

#### Scenario: Invalid policy rejected
- **WHEN** an administrator saves a document in which policy.tools.shell.mode is "maybe"
- **THEN** the save is rejected with a validation error that identifies the field

#### Scenario: Unknown key rejected
- **WHEN** an administrator saves a document that contains policy.tools.shell.colour
- **THEN** the save is rejected with a validation error that names that key

### Requirement: Policy Versions And Drafts
The policy service SHALL keep for each Group, and for the guardrails, at most one editable draft and a numbered history of published versions. Saving replaces the draft. Publishing turns the draft into the next version number, which MUST NOT be changed afterwards. Policy documents that are in force are only ever published versions.

#### Scenario: Draft does not affect users
- **WHEN** an administrator saves a draft for "developers" and does not publish it
- **THEN** no user's effective policy changes and no policy-changed message is sent

#### Scenario: Publish creates a version
- **WHEN** an administrator publishes the draft of "developers" whose latest version is 3
- **THEN** version 4 exists, the draft is gone, and the publish is audited with the old and new version numbers

### Requirement: Cross-Key Validation
The policy service SHALL reject a policy document, and any effective policy, that breaks a rule spanning several keys. Currently there is one such rule: policy.scheduler.max_concurrent_runs must not be greater than policy.llm.max_concurrent_runs. The policy service checks the effective policy when it compiles a bundle, so a combination produced by several Groups is also caught.

#### Scenario: Document breaks the rule
- **WHEN** an administrator saves a document that sets scheduler.max_concurrent_runs 6 and llm.max_concurrent_runs 4
- **THEN** the save is rejected with an error that names both keys

#### Scenario: Scheduler concurrency above total
- **WHEN** the effective policy would have scheduler.max_concurrent_runs 4 and llm.max_concurrent_runs 2
- **THEN** compilation fails for the affected users, the previous bundle stays in force, and administrators see the conflict in simulation

#### Scenario: Publish would break the rule for a combination of Groups
- **WHEN** an administrator publishes a draft that sets scheduler.max_concurrent_runs 5, and users of the Groups "developers" and "power-users" are known to the fleet registry, whose combined effective policy would then have llm.max_concurrent_runs 3
- **THEN** the publish is refused with an error that names both keys and the Groups, and nothing changes

#### Scenario: Defaults count in the rule
- **WHEN** a Group sets scheduler.max_concurrent_runs 4 and no Group sets llm.max_concurrent_runs, whose default is 3
- **THEN** the combination breaks the rule and is refused as above

### Requirement: Policy Schema Ownership
The policy schema SHALL be the union of the policy keys that capability specs define as policy.<path>. Each key has exactly one owning capability. Its owning spec states the key's meaning, allowed values and default. The schema is versioned, and the service MUST reject keys that no capability defines.

#### Scenario: Key introduced by a capability
- **WHEN** a change adds policy.tools.computer.app_denylist to client/computer-use
- **THEN** the next schema version accepts that key, and documents that do not set it get the default stated in client/computer-use

#### Scenario: Schema available to administrators
- **WHEN** an administrator requests the policy schema
- **THEN** the policy service returns a JSON Schema generated from the same definition that validates documents, together with the schema version

### Requirement: Effective MCP Catalog In Bundle
The policy service SHALL include in each user's Policy bundle, as policy.mcp.servers, the control-plane/mcp-catalog entries that are enabled and assigned to the user's Groups, with their versions, hashes and tool risk annotations. It obtains them from control-plane/mcp-catalog and does not store them.

#### Scenario: Catalog entry added for a Group
- **WHEN** an administrator allows an MCP server for the Group "developers"
- **THEN** the next bundle for each developer contains that catalog entry, and a policy-changed push is sent

#### Scenario: Catalog entry disabled
- **WHEN** an administrator disables an MCP server
- **THEN** the next bundle of the affected users no longer contains it

### Requirement: Dispatch Rule Management
The policy service SHALL store the dispatch rules used by control-plane/task-dispatch, and let administrators list, add and remove them. The rules are never included in a Policy bundle.

#### Scenario: Administrator adds a rule
- **WHEN** an administrator adds a dispatch rule
- **THEN** control-plane/task-dispatch applies it to the next request, and the change is audited

### Requirement: Dispatch Rule Matching
A dispatch rule SHALL name an issuer (a user principal, an admin role, a member of a Group, the owner of a Group, or a registered service principal) and a target (a user principal, a Group, or all users). A request is authorized only when at least one rule matches its issuer and its target. With no matching rule the request is denied, whatever the issuer's admin role.

#### Scenario: Group owner targets own Group
- **WHEN** a rule allows the group-owner of "developers" to target "developers"
- **THEN** that owner's Remote task for a developer is authorized, and the same owner's task for a user outside the Group is not

#### Scenario: No rule
- **WHEN** no rule matches an issuer
- **THEN** that issuer's request is not authorized, whatever the issuer's admin role

#### Scenario: Registered system targets a Group
- **WHEN** a rule allows the service principal "ci-bot" to target "developers"
- **THEN** a Remote task from "ci-bot" for a user who is a member of "developers" is authorized

### Requirement: Org Guardrail Layer
The policy service SHALL maintain an org guardrail layer that applies to every user. Group policies MUST NOT override it.

#### Scenario: Guardrail applies to every Group
- **WHEN** the guardrail sets a value and a Group policy sets a different value for the same key
- **THEN** the effective policy uses the guardrail's bound, and the Group value is not used beyond it

### Requirement: Guardrail Strictness
A guardrail SHALL only make the effective policy stricter. It takes effect at once for everyone, with no staged rollout. A Group rule that is more permissive than a guardrail is ignored in the effective policy and flagged in simulation.

#### Scenario: Guardrail denies a domain
- **WHEN** the guardrail denies egress to "*.example-filesharing.com" and the developers policy allows it
- **THEN** the effective policy for developers denies that domain, and simulation shows the conflict

#### Scenario: Guardrail bounds a number
- **WHEN** the guardrail sets policy.tools.shell.limits.memory_mb to at most 4096 and a Group sets 8192
- **THEN** the effective value is 4096, and simulation shows the conflict

#### Scenario: Guardrail forces approval
- **WHEN** the guardrail sets the rule `browser.*` to approve and a Group sets `browser.click` to allow
- **THEN** the effective rule for `browser.click` is approve

### Requirement: Guardrail Application
For each key the policy schema SHALL define how a guardrail applies to it: it adds deny entries, makes a decision stricter, bounds a number or an ordered level, restricts a list to listed values, or forces a value. The service MUST reject a guardrail key that has no such meaning. A guardrail also applies to a key that no Group sets, when the default of that key would break it.

#### Scenario: Guardrail key without meaning
- **WHEN** an administrator saves a guardrail that sets policy.filesystem.read
- **THEN** the save is rejected with a validation error that names the key

#### Scenario: Guardrail restricts a list
- **WHEN** the guardrail sets policy.skills.publish to shared and a Group sets shared, group and org
- **THEN** the effective policy.skills.publish is shared alone

#### Scenario: Guardrail bounds a default
- **WHEN** the guardrail sets policy.llm.budget.tokens to at most 400000 and no Group sets it, whose default is higher
- **THEN** the effective policy.llm.budget.tokens is 400000

### Requirement: Effective Policy Composition
The policy service SHALL compile each user's effective policy from the guardrails and all the user's Group policies. Guardrails bound everything in the result.

#### Scenario: Compiled from all Groups
- **WHEN** a user is a member of three Groups and a guardrail applies
- **THEN** the effective policy is compiled from the guardrail and all three Group policies in one pass

### Requirement: Deny And Union Rules
An explicit deny SHALL always win across Groups, and list entries SHALL be unioned across Groups. In decision maps (tools.rules, tools.risk, unattended.rules) a key that any Group denies is deny, and any other key takes the value of the highest-priority Group that sets it.

#### Scenario: Deny in any group wins
- **WHEN** one Group allows a filesystem path and another of the user's Groups denies it
- **THEN** the effective policy denies that path

#### Scenario: Conflicting decision rules
- **WHEN** "developers" (priority 20) sets `browser.*` to allow and "standard" (priority 10) sets `browser.*` to approve
- **THEN** the effective rule for `browser.*` is allow, and it would be deny if either Group denied it

### Requirement: Scalar Precedence
For each scalar the highest-priority Group that sets it SHALL decide it, separately from other scalars. When two Groups of equal priority set different values, the Group whose name sorts first decides, so that the result is deterministic.

#### Scenario: Two groups
- **WHEN** a user is in "power-users" (priority 10, shell mode off) and "developers" (priority 20, shell mode sandboxed)
- **THEN** the effective shell mode is sandboxed

#### Scenario: Equal priority
- **WHEN** two Groups of equal priority set different values for the same scalar
- **THEN** the Group whose name sorts first decides it, so that the result is deterministic

### Requirement: Sparse Effective Policy
The effective policy SHALL contain only the keys that some Group or guardrail sets. The client applies the default stated by the owning spec for every other key. A map of values that belong together, such as policy.llm.models, is decided as a whole by the highest-priority Group that sets it.

#### Scenario: Unset key
- **WHEN** no Group and no guardrail sets policy.llm.budget.embed_daily_tokens
- **THEN** the effective policy does not contain that key

#### Scenario: Model mapping from the highest-priority Group
- **WHEN** "developers" (priority 20) sets policy.llm.models for main, vision and small, and "standard" (priority 10) sets it for main and embed
- **THEN** the effective policy.llm.models is the mapping of "developers" alone, so embed is unmapped for those users

### Requirement: Signed Bundles
The policy service SHALL sign each effective policy with the policy signing key and bind it to the user principal, and it SHALL publish the public verification keys at a well-known endpoint.

#### Scenario: Signing key rotation
- **WHEN** an administrator rotates the policy signing key
- **THEN** the well-known endpoint publishes both the new key and the previous one, so bundles signed with either verify

### Requirement: Bundle Versioning And Expiry
Each Policy bundle SHALL carry a monotonically increasing version and SHALL expire after the bundle validity, a control-plane server setting with a default of 7 days.

#### Scenario: Bundle fetch
- **WHEN** an authenticated client requests its policy
- **THEN** it receives a signed bundle for its user principal with a version and an expiry time

### Requirement: Bundle Reuse And Compile Failure
When the effective policy of a user has not changed and the issued bundle is still within the first half of its validity, the policy service SHALL return that same bundle and version. Otherwise it issues a new bundle with the next version. If the effective policy cannot be compiled, it MUST NOT issue a bundle, and it returns the previously issued bundle for as long as that bundle is valid.

#### Scenario: Unchanged policy keeps the version
- **WHEN** a client fetches its bundle twice and nothing that affects its effective policy changed in between
- **THEN** both responses carry the same bundle and version

#### Scenario: Compilation fails
- **WHEN** a client fetches its bundle while its effective policy no longer compiles
- **THEN** it receives the previously issued bundle while that is still valid, and an error that names the conflict once it is not

### Requirement: Change Notification
When a change affects users' effective policies, the policy service SHALL send a policy-changed message through control-plane/fleet-registry to the affected users or Groups. The policy service is the only sender of policy-changed.

#### Scenario: Group policy edited
- **WHEN** an administrator publishes a new version of the developers policy
- **THEN** connected clients of developers receive policy-changed and fetch the new bundle

### Requirement: Notification Scope
The policy service SHALL treat as affecting users' effective policies the changes to policy documents, the guardrails, the role mappings and direct assignments, and the mcp-catalog Group assignments and entries. A role-mapping change affects every installation, because the policy service does not know which users hold each role. A direct Group assignment affects only the assigned user.

#### Scenario: Group mapping changed
- **WHEN** an administrator replaces the role mappings
- **THEN** every installation receives policy-changed

#### Scenario: Direct assignment changed
- **WHEN** an administrator assigns a user to a Group directly
- **THEN** only that user's installations receive policy-changed

### Requirement: Staged Rollout
The policy service SHALL let administrators publish a policy version to a pilot Group, or to a percentage of a Group's users, before publishing it to everyone. A user is in a percentage when a stable hash of the user principal modulo 100 is below that percentage. Users who are not selected stay on the version that was in force. Administrators promote the version to everyone, or cancel the rollout. The policy service can also roll back to the previous version.

#### Scenario: Pilot then full rollout
- **WHEN** an administrator publishes a new version to 10% of developers and later promotes it
- **THEN** only the selected 10% receive it first, and all developers receive it after promotion

#### Scenario: Pilot Group
- **WHEN** an administrator publishes a new version of "developers" to the pilot Group "pilot-users"
- **THEN** only the members of both "developers" and "pilot-users" receive it, and policy-changed is sent only to them

#### Scenario: Rollback
- **WHEN** an administrator rolls back a policy
- **THEN** affected users receive a new bundle version whose content equals the previous policy

### Requirement: Policy Simulation
The policy service SHALL compute and show the effective policy for a given user, or a given set of Groups, without publishing it. The result shows which Group or guardrail produced each value, every guardrail conflict, any compilation error, and the network enforcement level reported by the user's clients. Administrators can choose to simulate with the drafts in place of the published versions. Clients report their network enforcement level with the heartbeat.

#### Scenario: Simulate a user
- **WHEN** an administrator simulates the policy for a named user
- **THEN** the effective policy is shown with the source of each rule

#### Scenario: Simulate a draft
- **WHEN** an administrator simulates a set of Groups with drafts included
- **THEN** the result shows the effective policy as if every draft were published, and no user is affected

### Requirement: Policy Diff
The policy service SHALL show the differences between any two versions of a Group policy or guardrail, and between a draft and the published version. Differences are shown per key, and the policy service SHALL also show the resulting change in the effective policy of a selected user.

#### Scenario: Review a draft
- **WHEN** an administrator opens the diff between the developers draft and the published version
- **THEN** every added, removed and changed key is listed with its old and new value

### Requirement: Policy Audit
The policy service SHALL record every create, edit, publish, rollout and rollback of policies, Group mappings and guardrails. Each record holds the administrator, time, before and after content, and reason.

#### Scenario: Audit trail
- **WHEN** an administrator changes a Group mapping
- **THEN** an audit record with the old and new mapping and the administrator's identity is stored

### Requirement: Administration API
The policy service SHALL expose all of its functions through an authenticated administration API. Access is restricted to the administrator roles defined by control-plane/admin-console: platform-admin for everything, and group-owner for the policies of the Groups the user owns.

#### Scenario: Non-admin access
- **WHEN** a user without an administrator role calls the administration API
- **THEN** the request is refused

### Requirement: Policy Retrieval By Clients
The policy service SHALL let each authenticated user fetch the Policy bundle for their own user principal, compiled for the Groups that control-plane/identity determines for that user. It MUST refuse service principals.

#### Scenario: Group membership changed
- **WHEN** a user is assigned to a Group that the previous bundle did not include
- **THEN** the next fetch returns a bundle compiled for the new set of Groups, with a higher version
