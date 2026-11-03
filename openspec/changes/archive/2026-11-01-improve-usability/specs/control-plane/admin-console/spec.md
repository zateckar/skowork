# Spec Delta

## ADDED Requirements

### Requirement: Structured Policy Editor
The admin console SHALL let an admin edit a policy document through typed controls generated from the policy schema of control-plane/policy-service (choices, switches, number fields and editable lists), grouped by top-level policy key, each showing its owning capability and allowed range. A key that is not set shows its default as inherited. The raw JSON view stays available, and both views edit the same draft. Validation errors appear next to the affected control.

#### Scenario: Enable computer use without JSON
- **WHEN** an admin switches computer use to allow for a Group in the structured editor, saves the draft and publishes
- **THEN** the published document contains policy.tools.computer.mode set to allow, and the raw JSON view shows the same document

#### Scenario: Error next to the control
- **WHEN** an admin enters an invalid host pattern in the network allow list and saves
- **THEN** the validation error is shown next to that list, and nothing is published

### Requirement: Every Known Group Is Listed
The policy and model views SHALL list every Group known to the control plane: Groups defined in control-plane/policy-service, Groups with a policy version or draft, Groups with direct members or identity-provider role mappings, and `everyone`. A Group that is not defined is marked as such and can be given a policy without defining it first.

#### Scenario: Group with members only
- **WHEN** a platform-admin assigns alice to the Group engineering, which has no policy, and opens the models view
- **THEN** engineering can be chosen, and saving its model mapping creates its policy draft

### Requirement: Console Usability
The admin console SHALL be usable at window widths from 1280 px: cards wrap to fewer columns instead of shrinking, and wide tables scroll inside their card without covering other content. Every input has a visible label, every icon-only button has an accessible name, and every destructive action (revoke, lift, remove, retire, delete) asks for confirmation first.

#### Scenario: Narrow window
- **WHEN** the Identity view is shown in a 1280 px wide window
- **THEN** no card content overlaps another card, and the deny-list table scrolls inside its card

#### Scenario: Confirm revocation
- **WHEN** an admin clicks Revoke for an installation
- **THEN** the console asks for confirmation, and nothing is revoked if the admin cancels
