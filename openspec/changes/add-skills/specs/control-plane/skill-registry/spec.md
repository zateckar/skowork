# Spec Delta

## Purpose

Defines the central registry that stores, versions, reviews, signs, distributes and revokes Skills shared beyond a single user, and that reports metadata-only usage back to authors.

## ADDED Requirements

### Requirement: Skill Storage And Versioning
The skill registry SHALL store every published Skill version immutably. Each version records its author, scope, semantic version, content digest and publication time. Publishing an existing version number with different content MUST be rejected.

#### Scenario: Republish same version
- **WHEN** an author publishes version 1.2.0 of a Skill that already has a different 1.2.0
- **THEN** the registry rejects the publish and asks for a new version number

### Requirement: Sharing Scopes
The skill registry SHALL support these sharing scopes:
- shared: named users
- group: one or more Groups
- org: all users

Each Skill version is distributed only to users within its scope. Publishing is allowed only to the scopes permitted by the author's policy.skills.publish.

#### Scenario: Group-scoped skill
- **WHEN** a Skill is published to the Group "developers"
- **THEN** only members of that Group receive it in their group tier

#### Scenario: Scope not permitted
- **WHEN** an author whose policy does not allow group publishing tries to publish to a Group
- **THEN** the registry rejects the publish

### Requirement: Automated Skill Scan
The skill registry SHALL scan every Skill version submitted for group or org scope. The scan looks for:
- scripts and the commands they run;
- network endpoints referenced;
- embedded secrets;
- tool and MCP requirements;
- known prompt-injection patterns.

Results are attached to the version. A version with embedded secrets MUST be rejected automatically.

#### Scenario: Secret detected
- **WHEN** a submitted Skill contains an API key in a script
- **THEN** the registry rejects the submission and tells the author where the secret was found

### Requirement: Org Review And Signing
The skill registry SHALL make an org-scope Skill version available only after a skill-reviewer approves it. On approval, the registry signs the version's content digest. Reviewers MUST NOT approve their own submissions.

#### Scenario: Approval signs version
- **WHEN** a skill-reviewer approves a submitted org Skill version
- **THEN** the registry signs it, publishes it to the org scope, and notifies the author

#### Scenario: Self-review blocked
- **WHEN** the author of a Skill version tries to approve it
- **THEN** the registry refuses the approval

### Requirement: Public Skill Import
The skill registry SHALL accept externally sourced Skills, such as those from agentskills.io, only as submissions to org review. A platform-admin gives either the HTTPS URL of the package, which the registry downloads, or the package itself, with an optional source URL. The licence is the one given, or else the license field of SKILL.md; an import without either is refused. The source URL, when known, and the licence are recorded on the version.

#### Scenario: Import public skill
- **WHEN** a platform-admin imports a public Skill
- **THEN** it enters the org review queue with its source URL and licence, and it is not distributed until approved

#### Scenario: Import from a URL
- **WHEN** a platform-admin imports a public Skill by the HTTPS URL of its zip archive
- **THEN** the registry downloads the archive within the package size limit and validates it as if it had been uploaded

#### Scenario: URL and package both given
- **WHEN** a platform-admin gives a package and asks the registry to download from a URL in the same import
- **THEN** the import is refused with a validation error

#### Scenario: Licence from SKILL.md
- **WHEN** a platform-admin uploads a package whose SKILL.md has license MIT and gives no licence
- **THEN** the version is recorded with the licence MIT

### Requirement: Skill Distribution
The skill registry SHALL give each client the list of Skill versions in its user's shared, group and org scopes, and the content of each version on request, authenticated with the user's access token.

#### Scenario: Client sync
- **WHEN** a client requests its Skill list
- **THEN** the registry returns only versions within the user's scopes, with digests and signatures

### Requirement: Skills Changed Notification
The skill registry SHALL send a skills-changed push through control-plane/fleet-registry to the affected users or Groups when a Skill version becomes available in a scope, is updated, or is withdrawn by its author.

#### Scenario: New group skill version
- **WHEN** version 1.3.0 of a Skill published to the Group "developers" becomes available
- **THEN** connected clients of developers receive skills-changed and sync their Skill list

### Requirement: Skill Revocation
The skill registry SHALL let the author, for shared and group scopes, or a platform-admin or skill-reviewer, for any scope, revoke a Skill or a specific version. It SHALL then send a skill-revoked push to affected clients through control-plane/fleet-registry. It SHALL also report the revocation in every later Skill sync.

#### Scenario: Revoke org skill
- **WHEN** a skill-reviewer revokes an org Skill
- **THEN** the registry marks it revoked, stops distributing it, and sends skill-revoked to all clients

### Requirement: Usage Counts For Authors
The skill registry SHALL show each author aggregate usage counts for their Skills: installs, runs and failure rate per week. These counts MUST NOT identify individual users.

#### Scenario: Author views usage
- **WHEN** an author opens the usage view for their Skill
- **THEN** they see weekly install and run counts with no user names

### Requirement: Skill Package Validation
The skill registry SHALL accept a Skill as a zip archive with SKILL.md at its root, or inside one top-level folder. SKILL.md MUST have a kebab-case name, a description and a semantic version, given as version or as metadata.version (client/skills Agent Skills Compatibility). The registry MUST reject an archive with an unknown top-level entry, an unsafe path, too many entries, or too much content, and MUST reject a package larger than the configured limit.

#### Scenario: Missing version
- **WHEN** an author publishes a package whose SKILL.md has no version
- **THEN** the registry rejects it with a validation error that names the field

#### Scenario: Version in metadata
- **WHEN** an author publishes a package whose SKILL.md has no version field and whose metadata block holds version 1.0.0
- **THEN** the registry accepts it as version 1.0.0

#### Scenario: Unsafe archive
- **WHEN** an archive contains an entry whose path leaves the Skill folder
- **THEN** the registry rejects the package

### Requirement: Skill Name Ownership
The skill registry SHALL give the name of a Skill to the user who first publishes it. Only that user MAY publish further versions under the name. Publishing a version whose number exists with the same content and the same audience returns the existing version.

#### Scenario: Name taken
- **WHEN** a second user publishes a Skill under a name that another user owns
- **THEN** the registry rejects the publish and names the conflict

### Requirement: Publish Audience
A shared Skill version SHALL name the users who receive it, and a group Skill version SHALL name the Groups that receive it. The author MUST be a member of every named Group, unless the author is a platform-admin. An org Skill version has no named audience.

#### Scenario: Group the author is not in
- **WHEN** an author publishes a group Skill for a Group they are not a member of
- **THEN** the registry rejects the publish

### Requirement: Scan Findings
The skill registry SHALL attach every finding of the scan to the version, with its kind, severity, file and line, and MUST NOT store or show the value of a secret. A version with a secret finding is rejected in every scope. The scan also records the commands and URLs found in scripts, the declared tools and MCP servers, and personal data patterns.

#### Scenario: Secret in a shared Skill
- **WHEN** an author shares a Skill whose script contains an API key
- **THEN** the registry rejects it with the file and line, and without the key

#### Scenario: Findings for a reviewer
- **WHEN** a reviewer opens a submitted org Skill version
- **THEN** the findings, the declared tools, the declared risk and the script commands are shown with the version

### Requirement: Review Queue
The skill registry SHALL list org Skill versions that await review to platform-admins and skill-reviewers, who can approve or reject each with a comment. A rejection requires a comment. Both outcomes notify the author, and a rejected version is never distributed.

#### Scenario: Reject with comment
- **WHEN** a skill-reviewer rejects a submitted version with a comment
- **THEN** the version is rejected, the author is notified, and nobody receives it

### Requirement: Signing Keys
The skill registry SHALL sign with a dedicated ES256 key ring that is separate from the token and policy keys, and SHALL publish its public keys at a well-known endpoint. A key that was rotated out stays published, so that every signature ever issued verifies.

#### Scenario: Verify a signature
- **WHEN** a client verifies the signature of an org Skill version against the published keys
- **THEN** the signature is valid and its content digest equals the digest of the version

### Requirement: Author Withdrawal
The skill registry SHALL let the author withdraw a version that is not revoked. A withdrawn version is no longer distributed, and the registry sends skills-changed to the users in its scope. A withdrawal is not a revocation and is not listed as one.

#### Scenario: Withdraw a version
- **WHEN** an author withdraws version 1.3.0 of a group Skill
- **THEN** the next Skill list of the Group no longer contains it, and connected clients receive skills-changed

### Requirement: Usage Reporting
The skill registry SHALL accept aggregate counts of installs, runs and failures per Skill from authenticated clients and add them to the current week, without storing which user or installation reported them.

#### Scenario: Client reports counts
- **WHEN** a client reports 3 runs and 1 failure for a Skill
- **THEN** the counts of the current week for that Skill grow by 3 runs and 1 failure, and no identity is stored with them
