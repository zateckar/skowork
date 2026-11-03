# Spec Delta

## Purpose

Adds to control-plane/identity the service principals that submit Remote tasks, such as Jira or a CI system. These requirements moved here from add-client-foundation, because task dispatch is their first consumer. Service principals are systems, not users, so they never receive Groups or admin roles.

## ADDED Requirements

### Requirement: Service Principal Tokens
The control plane SHALL accept client-credentials tokens only from clients registered as service principals, and only for the APIs that each registration permits. The registration is the allow-list for these tokens; the allowed-clients list of Identity Provider Trust applies only to user access tokens.

#### Scenario: Registered system calls an API
- **WHEN** a registered service principal calls an API listed in its registration
- **THEN** the call is authenticated as that service principal

#### Scenario: No user identity
- **WHEN** a service-principal token is authenticated
- **THEN** the request carries no user subject, no Groups and no admin roles

#### Scenario: Unregistered system
- **WHEN** a client-credentials token from an unregistered client calls any API
- **THEN** the call is refused with 403 and audited

#### Scenario: Service principal calls a user-only API
- **WHEN** a registered service principal calls an API that is not in its registration, or that requires a user
- **THEN** the call is refused with 403 and audited
