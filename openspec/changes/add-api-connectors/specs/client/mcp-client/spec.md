# Spec Delta

## Purpose

The settings page that lists the user's Connectors is called Connectors. Its MCP part is specified here; the API part is in client/api-connectors.

## MODIFIED Requirements

### Requirement: Health And Diagnostics
The client SHALL show each catalog MCP server's state on the Connectors page in settings and in the status bar: connected, idle, failed or unavailable, plus the number of usable tools. A server that connects but exposes no usable tools MUST be reported as a warning with the reason.

#### Scenario: Server with no usable tools
- **WHEN** a catalog MCP server connects but every tool is excluded by policy
- **THEN** the Connectors page shows the server with a warning "no usable tools" and names the policy exclusion as the reason
