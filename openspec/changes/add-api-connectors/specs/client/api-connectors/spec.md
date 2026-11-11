# Spec Delta

## Purpose

Defines how the client offers the selected operations of an API connector to the agent, sends their calls to the control plane, and reports their status. The client never contacts an API host.

## ADDED Requirements

### Requirement: API Connector Tools
The client SHALL offer each selected operation of an API connector from the Policy bundle to the agent as a tool with the id api:<connector>/<operation>. Its description is the operation's description, labelled as set by the connector's administrator, and its input is the schema in the bundle. The client MUST NOT offer an operation the bundle does not select, and MUST NOT run an entry of kind api as an MCP server.

#### Scenario: Selected operations offered
- **WHEN** the bundle selects two operations of the connector "orders-api"
- **THEN** the agent sees the tools api:orders-api/getOrder and api:orders-api/createOrder, and no other operation of that connector

### Requirement: Connector Calls Through Control Plane
The client SHALL send an API tool call to the control plane, which executes it under control-plane/api-connectors Connector Call Authorization. Before sending, the client SHALL check the arguments against the bundle's schema and refuse arguments that do not match. The client MUST NOT contact the connector's host.

#### Scenario: Invalid argument before sending
- **WHEN** the agent calls api:orders-api/getOrder without its required path parameter
- **THEN** the call fails at the client with a message that names the parameter, and no request is sent to the control plane

### Requirement: Risk And Approval
The client SHALL declare the risk class from the bundle for each API tool call to client/policy-enforcement, which decides allow, approve or deny. A call that is not allowed MUST NOT be sent to the control plane. Unattended runs follow policy.unattended.allowed_risk.

#### Scenario: Write call needs approval
- **WHEN** the agent calls a POST operation with risk class external and policy.tools.rules approves it
- **THEN** the user is asked to approve the call, and the call is sent only after approval

### Requirement: API Results Are Untrusted
The client SHALL return an API response to client/agent-runtime as untrusted content with the provenance api:<connector>/<operation>. client/agent-runtime applies the tool-result cap, and client/policy-enforcement guarantees that the content cannot grant permissions.

#### Scenario: Result provenance
- **WHEN** api:orders-api/getOrder returns an order
- **THEN** the content enters the context as untrusted with provenance "api:orders-api/getOrder"

### Requirement: Connector Errors
When the control plane refuses or fails a call, the client SHALL return the control plane's reason as the tool error. The client MUST NOT retry a call on its own. A busy or unavailable error SHALL be shown as such, so that the user can ask again later.

#### Scenario: Busy connector
- **WHEN** the control plane answers a call with the busy error
- **THEN** the tool result says that the connector is busy and that the call was not made, and no retry is sent

### Requirement: Connector Status
The client SHALL show each API connector in the user's Policy bundle on the Connectors page, with its state, its number of usable tools, and the reason when none is usable. The page SHALL name the connector's kind as API.

#### Scenario: No usable tools
- **WHEN** the bundle holds an API connector whose selected operations are all gone
- **THEN** the Connectors page shows the connector as a warning with the reason "no usable tools"

### Requirement: Personal Token Entry
For an API connector in user-pat mode, the Connectors page SHALL show the state from the control plane's connector listing, with the help text and help link, and SHALL let the user add, replace and remove their token through the control plane's user API. The client MUST NOT store, log, cache or show a token.

#### Scenario: Needs-token state with action
- **WHEN** the control plane lists a user-pat connector with the state needs-token
- **THEN** the Connectors page shows the help text and an "Add token" action, and the connector has no usable tools

#### Scenario: Token added from the Connectors page
- **WHEN** the user enters a token for a connector in state needs-token and confirms
- **THEN** the client sends the token to the control plane once, clears the field, and shows the connector as ready after the next policy update

#### Scenario: Token never shown again
- **WHEN** the user reopens the Connectors page after a token was set
- **THEN** the page shows that a token is set and when, and the token text does not appear anywhere in the client

#### Scenario: Rejected token prompts for replacement
- **WHEN** the control plane reports the state token-rejected for a connector
- **THEN** the page shows "token rejected, replace it" and offers the Add token action
