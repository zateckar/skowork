# Spec Delta

## Purpose

Remote servers are reached with identity-provider tokens: the administrator's during discovery, the user's on clients.

## MODIFIED Requirements

### Requirement: Catalog Entry Rules
The catalog SHALL give each entry an id of lower-case letters, digits and hyphens, because the id forms the tool id mcp:<server>/<tool>. A remote entry has an HTTPS URL and an authentication mode: user-token, where the caller's identity-provider access token is presented (control-plane/identity Identity Provider Tokens Only), or none. A local entry names an uploaded package by hash. The catalog MUST reject an entry that breaks these rules.

#### Scenario: Invalid id
- **WHEN** an administrator registers an entry with the id "Jira/Corp"
- **THEN** the registration is rejected with a validation error that names the id

#### Scenario: Unknown package
- **WHEN** an administrator registers a local entry with a hash for which no package was uploaded
- **THEN** the registration is rejected with a validation error that names the hash

#### Scenario: Unknown authentication mode
- **WHEN** an administrator registers a remote entry with the authentication mode audience-token
- **THEN** the registration is rejected with a validation error that names the authentication mode

### Requirement: Entry Delivered In The Policy Bundle
The catalog SHALL supply, for a given set of Groups, the enabled entries assigned to any of them, with what the client needs to connect (URL and authentication mode, or package hash, arguments and environment), the tool include and exclude lists including tools that await review, and the risk annotations. control-plane/policy-service embeds them as policy.mcp.servers. Environment values are visible to every user of the entry and MUST NOT hold secrets.

#### Scenario: Entry in the bundle
- **WHEN** an enabled entry "jira-corp" is assigned to the Group "developers"
- **THEN** the policy.mcp.servers of every developer's bundle contains "jira-corp" with its URL, its authentication mode, its include and exclude lists and its risk annotations

#### Scenario: Disabled entry
- **WHEN** an administrator disables "jira-corp"
- **THEN** the next bundle of every affected user no longer contains it

### Requirement: Tool Discovery Snapshot
The catalog SHALL connect to remote servers on registration, on update and when an administrator repeats the discovery, and record a snapshot of their tool list. For an entry with user-token authentication it presents the identity-provider access token of the administrator whose request started the discovery, and it does not discover without such a request. Tools that are new since the last snapshot are flagged for administrator review and are excluded until reviewed.

#### Scenario: Server adds a tool
- **WHEN** a remote server starts exposing a tool that is not in the snapshot
- **THEN** the tool is flagged in the admin console and is not offered to clients until an administrator includes it

#### Scenario: Discovery with the administrator's token
- **WHEN** a platform-admin registers a remote entry with user-token authentication
- **THEN** the discovery requests carry the platform-admin's identity-provider access token as the bearer token

## REMOVED Requirements

### Requirement: Remote Audience Authorization
**Reason**: There is no token exchange whose audiences need authorizing (control-plane/identity Identity Provider Tokens Only).
**Migration**: Group Assignment still decides which servers reach a user's Policy bundle; the MCP server authorizes the user from the identity-provider token.
