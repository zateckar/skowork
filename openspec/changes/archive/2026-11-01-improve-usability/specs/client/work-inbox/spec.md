# Spec Delta

## ADDED Requirements

### Requirement: Policy Page
Settings SHALL include a Policy page that shows the summary required by client/policy-enforcement Policy Visibility To User, covering Groups, version and expiry, model roles, tool decisions and modes, filesystem bounds, network rules and enforcement level, and unattended rules. Each setting shows the Group or guardrail it comes from where the bundle says so. The page offers Refresh, which fetches the policy now.

#### Scenario: Computer use off shown
- **WHEN** the user opens the Policy page while policy.tools.computer.mode is off
- **THEN** the page shows computer use as off, with the source of that setting

### Requirement: Files Settings
The Files section of Settings SHALL list the user's folder grants and let the user add a folder through the system folder picker, choose read-only or read-write, and revoke a grant, within the bounds of client/policy-enforcement User Folder Grants. A refused grant shows the explanation that names the policy.

#### Scenario: Grant a folder
- **WHEN** the user adds a folder inside policy.filesystem.grantable as read-only
- **THEN** the grant is listed, and the agent can read files in that folder
