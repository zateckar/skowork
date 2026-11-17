# Spec Delta

## ADDED Requirements

### Requirement: Sampling Defaults Administration
In the Connections view, the admin console SHALL let platform-admins set and clear each chat model's sampling defaults (control-plane/llm-gateway Sampling Defaults Setting). An empty field stores no value. A refused value is shown with the gateway's reason.

#### Scenario: Set recommended values
- **WHEN** a platform-admin enters temperature 1.0, top_p 0.95 and top_k 20 for a chat model and saves
- **THEN** the model's settings show the three values, and requests to the model carry them
