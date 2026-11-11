# Spec Delta

## Purpose

The admin console manages Connectors of both kinds from one view. LLM connections keep their own view and name.

## MODIFIED Requirements

### Requirement: Grouped Navigation
The admin console SHALL show the views the admin's roles allow in a side navigation, grouped by task: Overview (Dashboards), Users and devices (Identity, Fleet), Policies (Policies, Models and quotas), AI services (LLM connections, Connectors, Skill review, Team knowledge), Automation (Remote tasks) and Compliance (Audit log, Individual access, Access records). A group with no allowed view is not shown.

#### Scenario: Platform-admin navigation
- **WHEN** a platform-admin signs in
- **THEN** the navigation shows the groups Overview, Users and devices, Policies, AI services and Automation with their views, and no Compliance group

#### Scenario: Connectors in navigation
- **WHEN** a platform-admin opens the console
- **THEN** the AI services group lists "Connectors", and the view that was labelled "MCP catalog" opens under that label

## ADDED Requirements

### Requirement: Connector Administration
The Connectors view SHALL list every Connector with its kind (MCP or API), its state and its number of usable tools. A platform-admin SHALL add an API connector by uploading its OpenAPI file, and the console SHALL show the report of control-plane/api-connectors Operation Usability for each operation, with the reason for each unusable one, before any operation can be selected.

#### Scenario: Upload with one bad operation
- **WHEN** a platform-admin uploads a document in which one operation has the description "TODO"
- **THEN** the report lists that operation as not usable with its reason, and the other operations stay selectable

### Requirement: Connector Selection And Risk
For an API connector the console SHALL let a platform-admin select the usable operations to expose, set each one's risk class and reason, choose the authentication mode (none, api-key, user-token or user-pat), and enter an api-key write-only. A save that the control plane refuses SHALL keep the previous values and show the control plane's reason next to the field. Descriptions of operations and of the API are shown as read-only text from the document; the console MUST NOT offer to edit them.

#### Scenario: Refused selection shown
- **WHEN** a platform-admin saves a selection that includes an unusable operation
- **THEN** the console keeps the previous selection and shows the reason next to the operation

### Requirement: Personal Access Token Settings In Console
For an API connector in user-pat mode the console SHALL show fields for the request header, the scheme, the help text and the help link, and SHALL send them to the control plane as control-plane/api-connectors Personal Access Token Settings describes. The console MUST NOT show any user's token, and MAY show only how many users have a token set.

#### Scenario: Help text required
- **WHEN** a platform-admin leaves the help text empty for a user-pat connector and saves
- **THEN** the save is refused and the console shows the control plane's reason next to the help text field
