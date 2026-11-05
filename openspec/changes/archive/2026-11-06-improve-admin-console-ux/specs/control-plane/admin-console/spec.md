# Spec Delta

## ADDED Requirements

### Requirement: Grouped Navigation
The admin console SHALL show the views the admin's roles allow in a side navigation, grouped by task: Overview (Dashboards), Users and devices (Identity, Fleet), Policies (Policies, Models and quotas), AI services (LLM connections, MCP catalog, Skill review, Team knowledge), Automation (Remote tasks) and Compliance (Audit log, Individual access, Access records). A group with no allowed view is not shown.

#### Scenario: Platform-admin navigation
- **WHEN** a platform-admin signs in
- **THEN** the navigation shows the groups Overview, Users and devices, Policies, AI services and Automation with their views, and no Compliance group

### Requirement: View Orientation
The admin console SHALL mark the current view in the navigation and start the view with its title and a one-line description of its purpose.

#### Scenario: Open a view
- **WHEN** an admin opens the Fleet view
- **THEN** Fleet is marked as current in the navigation, and the view starts with the title Fleet and its purpose

### Requirement: Pending Review Count
The Skill review entry of the navigation SHALL show the number of Skill versions waiting for review when there are any.

#### Scenario: Pending reviews
- **WHEN** two Skill versions wait for review and a skill-reviewer opens the console
- **THEN** the Skill review entry shows the count 2

### Requirement: Action Feedback
The admin console SHALL show the outcome of every action where the admin can see it without scrolling: a success note appears as a notice fixed near the top of the window, and an action error appears there as well as next to the action. The notice can be dismissed and is announced to assistive technology.

#### Scenario: Revoke at the bottom of a long view
- **WHEN** an admin revokes a Skill version from the bottom of the Skill review view
- **THEN** a notice that the version was revoked is visible at the top of the window

### Requirement: Button Roles
The admin console SHALL draw buttons by their role: the main constructive action of a form as the primary button, actions that revoke, deny, lift, retire, remove or delete in the danger style, and every other action as a neutral button.

#### Scenario: Deny button
- **WHEN** an admin opens the deny list in the Identity view
- **THEN** the Deny button is drawn in the danger style

### Requirement: Skill Review Workflow
The Skill review view SHALL highlight the queue row whose version is under review, let the reviewer close the review, show Approve as the primary action, and list every Skill version, whatever its state, under All Skill versions with a state label.

#### Scenario: Open and close a review
- **WHEN** a reviewer opens the review of a queued version and then closes it
- **THEN** the row is highlighted while the review is open, and the review disappears when it is closed

### Requirement: Own Version Guidance
Before a reviewer decides on their own Skill version, the review SHALL say whether they can approve it, as control-plane/skill-registry Org Review And Signing defines: as the sole reviewer with a required justifying comment, or not at all because another reviewer must.

#### Scenario: Sole reviewer opens own import
- **WHEN** the only platform-admin opens the review of a Skill they imported
- **THEN** the review says that their approval will be recorded as a self-approval, and that a comment is required to approve

#### Scenario: Another reviewer exists
- **WHEN** an author opens the review of their own version while another skill-reviewer exists
- **THEN** the review says that another reviewer must approve it, and the Approve button is disabled
