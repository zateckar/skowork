# Spec Delta

## ADDED Requirements

### Requirement: LLM Connection Administration
The admin console SHALL let platform-admins add, edit, test, enable, disable and delete LLM connections of control-plane/llm-gateway, see each connection's status and discovered models, and set each model's capabilities. The credential field is write-only. In the Models view, the model for each role and Group is chosen from the discovered models, showing the connection and the model's kind; a model of the wrong kind for the role (for example a chat model for `embed`) is marked.

#### Scenario: Add a connection and use it
- **WHEN** a platform-admin adds a connection, the console lists its models, and the admin picks one of them as the `main` model of a Group and publishes
- **THEN** new Runs of that Group's users use the picked model through the new connection

#### Scenario: Delete a connection in use
- **WHEN** a platform-admin deletes a connection whose models are mapped in a published policy
- **THEN** the console names the Groups and roles that use it and asks for confirmation before deleting
