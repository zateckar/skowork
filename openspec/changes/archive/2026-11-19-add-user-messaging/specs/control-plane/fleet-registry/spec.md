# Spec Delta

## MODIFIED Requirements

### Requirement: Push Message Types
The fleet registry SHALL push to clients only these registered message types:
- revocation and minimum-version (this capability);
- policy-changed (control-plane/policy-service);
- skills-changed and skill-revoked (control-plane/skill-registry);
- remote-task, remote-task-claimed and remote-task-cancelled (control-plane/task-dispatch);
- chat-message, chat-changed and presence-changed (control-plane/user-messaging).

Client-to-server types are listed in Client Message Routing. New types are added through a change. Unknown types MUST be ignored and logged.

#### Scenario: MCP catalog change
- **WHEN** an MCP catalog entry changes
- **THEN** affected clients receive policy-changed from control-plane/policy-service, because the catalog is delivered inside the Policy bundle
