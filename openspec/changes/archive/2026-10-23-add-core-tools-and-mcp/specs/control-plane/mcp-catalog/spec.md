# Spec Delta

## Purpose

Defines the central registry of approved MCP servers: what each server is, how clients reach or run it, which Groups may use it, and how risky each of its tools is.

## ADDED Requirements

### Requirement: Catalog Entries
The catalog SHALL store one entry per approved MCP server, containing:
- a unique id and a display name;
- the owner;
- the kind: remote or local;
- for remote servers, the URL and the authentication mode;
- for local servers, the package source, SHA-256 hash, arguments and environment;
- the declared MCP specification version;
- tool include and exclude lists;
- tool risk annotations.

#### Scenario: Register local server
- **WHEN** an administrator registers a local server with a package and its SHA-256 hash
- **THEN** the entry is stored, and clients can fetch the package only by that hash

### Requirement: Group Assignment
The catalog SHALL assign each entry to one or more Groups. Group assignment is the only source of which MCP servers a user may use. A user's effective catalog is the union of the entries assigned to the user's Groups.

#### Scenario: Union across Groups
- **WHEN** a user is in Groups "developers" and "power-users", which have different assigned servers
- **THEN** the user's effective catalog contains the servers of both Groups

### Requirement: Tool Risk Annotations
The catalog SHALL let administrators annotate each tool of an entry with a risk class (read, write-local, external, destructive). Unannotated tools are delivered as external.

#### Scenario: Annotate tool
- **WHEN** an administrator marks mcp:jira-corp/search_issues as read
- **THEN** clients receive risk class read for that tool in the next policy bundle

### Requirement: Tool Discovery Snapshot
The catalog SHALL connect to remote servers on registration and update, and record a snapshot of their tool list. Tools that are new since the last snapshot are flagged for administrator review and are excluded until reviewed.

#### Scenario: Server adds a tool
- **WHEN** a remote server starts exposing a tool that is not in the snapshot
- **THEN** the tool is flagged in the admin console and is not offered to clients until an administrator includes it

### Requirement: Versioning And Change Push
Every change to an entry SHALL create a new catalog version with an audit record, and SHALL notify control-plane/policy-service of the affected Groups. control-plane/policy-service then includes the change in the affected users' Policy bundles and sends the policy-changed push. The catalog itself sends no push messages.

#### Scenario: Entry updated
- **WHEN** an administrator changes a server's hash to a new package version
- **THEN** a new catalog version is recorded, control-plane/policy-service is notified, and connected clients of the affected Groups receive a policy-changed push from it

### Requirement: Package Hosting
The catalog SHALL serve local server packages from an internal, authenticated package endpoint, addressed by hash, so clients never download MCP packages from the internet.

#### Scenario: Client fetches package
- **WHEN** a client needs a local server package
- **THEN** it downloads the package from the catalog's package endpoint using its access token, and the bytes match the catalog hash

### Requirement: Catalog Entry Rules
The catalog SHALL give each entry an id of lower-case letters, digits and hyphens, because the id forms the tool id mcp:<server>/<tool>. A remote entry has an HTTPS URL, an authentication mode (audience-token or none) and, for audience-token, the audience of its tokens, which defaults to the URL. A local entry names an uploaded package by hash. The catalog MUST reject an entry that breaks these rules.

#### Scenario: Invalid id
- **WHEN** an administrator registers an entry with the id "Jira/Corp"
- **THEN** the registration is rejected with a validation error that names the id

#### Scenario: Unknown package
- **WHEN** an administrator registers a local entry with a hash for which no package was uploaded
- **THEN** the registration is rejected with a validation error that names the hash

### Requirement: Entry Delivered In The Policy Bundle
The catalog SHALL supply, for a given set of Groups, the enabled entries assigned to any of them, with what the client needs to connect (URL and audience, or package hash, arguments and environment), the tool include and exclude lists including tools that await review, and the risk annotations. control-plane/policy-service embeds them as policy.mcp.servers. Environment values are visible to every user of the entry and MUST NOT hold secrets.

#### Scenario: Entry in the bundle
- **WHEN** an enabled entry "jira-corp" is assigned to the Group "developers"
- **THEN** the policy.mcp.servers of every developer's bundle contains "jira-corp" with its audience, its include and exclude lists and its risk annotations

#### Scenario: Disabled entry
- **WHEN** an administrator disables "jira-corp"
- **THEN** the next bundle of every affected user no longer contains it

### Requirement: Remote Audience Authorization
The catalog SHALL tell control-plane/identity which audiences a user may obtain a token for: the audience of every enabled remote entry with audience-token authentication that is assigned to one of the user's Groups.

#### Scenario: Audience of an assigned entry
- **WHEN** a user in "developers" requests a token for the audience of the enabled entry "jira-corp", which is assigned to "developers"
- **THEN** control-plane/identity issues the token

#### Scenario: Audience of an entry for another Group
- **WHEN** a user who is not in a Group of the entry requests a token for its audience
- **THEN** the request is refused

### Requirement: Discovery Failure
The catalog SHALL store a remote entry whose server could not be reached or did not answer the MCP handshake, with the discovery status failed and the reason, and SHALL keep it disabled until a discovery succeeds. An administrator can repeat the discovery at any time.

#### Scenario: Server unreachable
- **WHEN** an administrator registers a remote entry whose server does not answer
- **THEN** the entry is stored disabled with the discovery error, and enabling it is refused until a discovery succeeds

### Requirement: Tool Review Decisions
The catalog SHALL let an administrator include or exclude each flagged tool. An included tool is offered to clients, and an excluded tool is not. The tools of the first snapshot of an entry are the baseline and count as included, and only tools that appear later are flagged.

#### Scenario: Include a flagged tool
- **WHEN** an administrator includes the flagged tool "create_issue"
- **THEN** the next bundle offers the tool, and the decision is audited

### Requirement: Package Upload And Access
The catalog SHALL accept a package upload from a platform-admin, store it addressed by its SHA-256 hash, and serve it only to authenticated users who have, in their Groups, an enabled entry that names that hash. An upload larger than the configured size limit MUST be refused.

#### Scenario: Package for another Group
- **WHEN** a user whose Groups have no enabled entry for a package requests it by hash
- **THEN** the catalog answers as if the package does not exist

### Requirement: Catalog Administration
The catalog SHALL expose all of its functions through an authenticated administration API restricted to platform-admin, and SHALL record every change in the audit trail with the administrator, time, and the entry before and after.

#### Scenario: Non-admin access
- **WHEN** a user without an administrator role calls the catalog administration API
- **THEN** the request is refused
