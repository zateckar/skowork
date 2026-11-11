# Spec Delta

## ADDED Requirements

### Requirement: Model Fallback And Effort Administration
In the Connections view, the admin console SHALL let platform-admins set each chat model's reasoning effort, fallback model and fallback wait (control-plane/llm-gateway Reasoning Effort Setting, Fallback Model Setting). The fallback picker offers only the chat models the gateway would accept, with their connection. A refused value is shown next to its field with the gateway's reason, and the model list shows each model's effort and fallback.

#### Scenario: Set a fallback
- **WHEN** a platform-admin opens a chat model of the `primary` connection, picks the secondary model as its fallback, and saves
- **THEN** the model list shows the fallback, and requests for the model that time out are answered by the secondary model

#### Scenario: Refused effort shown
- **WHEN** a platform-admin saves a reasoning effort that the model rejects
- **THEN** the console keeps the previous value and shows the gateway's reason next to the field
