# Spec Delta

## ADDED Requirements

### Requirement: Team Knowledge Administration
The admin console SHALL show platform-admins the items in control-plane/team-knowledge by Group, with title, owner, Source kind, size and last update, and let them remove an item with a reason after confirmation.

#### Scenario: Review a Group's shared items
- **WHEN** a platform-admin opens team knowledge for the Group engineering
- **THEN** the console lists every item shared with engineering, with its owner and size
