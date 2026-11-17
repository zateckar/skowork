# Spec Delta

## ADDED Requirements

### Requirement: Keep-Alive Between Events
The gateway SHALL write a keep-alive comment to a streamed response only before the first upstream byte, or after a complete upstream event (an event that ends with a blank line). It MUST NOT write any bytes of its own inside an upstream event.

#### Scenario: Upstream stalls inside an event
- **WHEN** the upstream sends half of a `data:` line and then sends nothing for longer than the keep-alive interval
- **THEN** no keep-alive is written until the event is complete, and the client receives the event unchanged

#### Scenario: Upstream idle between events
- **WHEN** the upstream sends nothing for longer than the keep-alive interval after a complete event
- **THEN** the gateway writes a keep-alive comment
