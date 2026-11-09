# Spec Delta

## ADDED Requirements

### Requirement: Admin Directory
control-plane/identity SHALL list, for any holder of an administrator role, the user names and identity-provider roles it knows, for choosing them in the admin console. The list holds names only, never token contents or telemetry.

#### Scenario: Directory for a picker
- **WHEN** alice registered an installation, bob was assigned to the Group sales, and a platform-admin requests the directory
- **THEN** the users include alice and bob, and the bootstrap administrators are marked as such

### Requirement: Known Users And Roles
A user SHALL count as known when they registered an installation, are a direct Group member, hold an administrator role, own a Group, are on the deny list or are a bootstrap administrator. An identity-provider role SHALL count as known when it was seen in a user's token or appears in a role mapping.

#### Scenario: Role seen in a token
- **WHEN** carol's token carried the identity-provider role wb-sales and a platform-admin requests the directory
- **THEN** the identity-provider roles include wb-sales
