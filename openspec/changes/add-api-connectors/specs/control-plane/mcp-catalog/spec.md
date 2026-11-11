# Spec Delta

## Purpose

The catalog holds Connectors of two kinds: MCP servers and API connectors. The rules for Group assignment, versioning and audit are the same for both.

## MODIFIED Requirements

### Requirement: Catalog Entries
The catalog SHALL store one entry per approved Connector: a unique id, display name and owner; its kind (remote MCP server, local MCP server or API connector); its connection data; the MCP specification version of an MCP server; tool include and exclude lists; and tool risk annotations. Connection data is the URL and authentication mode of a remote MCP server, the package hash, arguments and environment of a local one, or the base URL, mode and document of an API connector.

#### Scenario: Register local server
- **WHEN** an administrator registers a local server with a package and its SHA-256 hash
- **THEN** the entry is stored, and clients can fetch the package only by that hash

#### Scenario: Register API connector
- **WHEN** an administrator registers an API connector with a valid OpenAPI document
- **THEN** the entry is stored with kind api, and its operations are listed as control-plane/api-connectors describes

### Requirement: Catalog Entry Rules
The catalog SHALL give each entry an id of lower-case letters, digits and hyphens, because the id forms the tool id mcp:<server>/<tool> or api:<connector>/<operation>. A remote MCP entry has an HTTPS URL and an authentication mode, user-token or none. A local MCP entry names an uploaded package by hash. An API connector has an HTTPS base URL and one of the modes none, api-key, user-token or user-pat; the settings of user-pat are checked as control-plane/api-connectors Personal Access Token Settings describes. The catalog MUST reject an entry that breaks these rules.

#### Scenario: Invalid id
- **WHEN** an administrator registers an entry with the id "Jira/Corp"
- **THEN** the registration is rejected with a validation error that names the id

#### Scenario: Unknown package
- **WHEN** an administrator registers a local entry with a hash for which no package was uploaded
- **THEN** the registration is rejected with a validation error that names the hash

#### Scenario: Unknown authentication mode
- **WHEN** an administrator registers a remote MCP entry with the authentication mode audience-token
- **THEN** the registration is rejected with a validation error that names the authentication mode

#### Scenario: HTTP base URL
- **WHEN** an administrator registers an API connector with an HTTP base URL outside development mode
- **THEN** the registration is rejected with a validation error that names the URL scheme

### Requirement: Entry Delivered In The Policy Bundle
The catalog SHALL supply, for a given set of Groups, the enabled entries assigned to any of them, with what the client needs to connect (URL and authentication mode, or package hash, arguments and environment), the tool include and exclude lists including tools that await review, and the risk annotations. control-plane/policy-service embeds them as policy.mcp.servers. Environment values are visible to every user of the entry and MUST NOT hold secrets.

#### Scenario: Entry in the bundle
- **WHEN** an enabled entry "jira-corp" is assigned to the Group "developers"
- **THEN** the policy.mcp.servers of every developer's bundle contains "jira-corp" with its URL, its authentication mode, its include and exclude lists and its risk annotations

#### Scenario: Disabled entry
- **WHEN** an administrator disables "jira-corp"
- **THEN** the next bundle of every affected user no longer contains it

## ADDED Requirements

### Requirement: API Connector Bundle Entry
The catalog SHALL embed an enabled API connector in policy.mcp.servers with kind api, its id, name, include and exclude lists, and the schemas and risk classes of its selected operations. It MUST NOT carry the base URL, a secret or the document.

For a connector in user-pat mode, the bundle carries operations only for a user with a usable token (control-plane/api-connectors Personal Access Token Gating), and the affected user's bundle is rebuilt when that token changes state.

#### Scenario: API connector in the bundle
- **WHEN** an enabled API connector "orders-api" with two selected operations is assigned to the Group "developers"
- **THEN** the policy.mcp.servers of every developer's bundle contains "orders-api" with kind api and those two operations, and none of them holds its base URL

#### Scenario: User-pat connector without a token
- **WHEN** an enabled user-pat connector "jira-api" is assigned to the Group "developers" and a developer has no token for it
- **THEN** that developer's bundle contains "jira-api" with the state needs-token and no operations

#### Scenario: Token set rebuilds the bundle
- **WHEN** that developer sets a token for "jira-api"
- **THEN** the developer's next bundle contains the selected operations of "jira-api", and no other user's bundle changes
