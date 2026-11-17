# Spec Delta

## ADDED Requirements

### Requirement: Sampling Defaults Setting
A platform-admin SHALL be able to store sampling defaults for each chat model: `temperature` (0 to 2), `top_p` (above 0 to 1), `top_k` (-1 or 1 to 1000), `min_p` (0 to 1), `presence_penalty` (-2 to 2) and `repetition_penalty` (above 0 to 2), each optional; other keys or values are refused with INVALID_REQUEST. The gateway SHALL add each stored value to a chat request routed to that model when the client's request does not contain that field. When a fallback model answers, its own defaults apply.

#### Scenario: Defaults applied
- **WHEN** the model mapped to `main` stores `temperature` 1.0 and `top_k` 20 and a client sends a chat request for `main` without sampling fields
- **THEN** the upstream receives `temperature: 1.0` and `top_k: 20`

#### Scenario: Client value kept
- **WHEN** the client's request contains `temperature: 0`
- **THEN** the upstream receives `temperature: 0` and the other stored defaults

#### Scenario: Out of range
- **WHEN** a platform-admin stores `top_p` 1.5
- **THEN** the setting is refused with INVALID_REQUEST and the previous defaults stay
