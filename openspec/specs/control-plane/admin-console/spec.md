# control-plane/admin-console Specification

## Purpose
The web console administrators use to author and publish policies, manage Groups, models, LLM connections, the MCP catalog, Skill review and the fleet, and to read dashboards.

## Requirements

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

### Requirement: Console Entry Point
The control plane SHALL serve the admin console at `/admin/`. When the console is built, a request for the root path `/` SHALL be redirected to `/admin/` with its query string unchanged, so a sign-in response sent to the root completes the sign-in. When WORKBENCH_ADMIN_REDIRECT_URI is not set, the console SHALL use `<console origin>/admin/` as its sign-in redirect URI.

#### Scenario: Open the root path
- **WHEN** an admin opens `https://<control plane>/` in a browser
- **THEN** the browser is redirected to `https://<control plane>/admin/` and the console loads

#### Scenario: Sign-in response at the root
- **WHEN** the redirect URI is `https://<control plane>` and the identity provider sends the browser to `https://<control plane>/?state=s&code=c`
- **THEN** the browser is redirected to `https://<control plane>/admin/?state=s&code=c`, and the console exchanges the code for a token

#### Scenario: Default redirect URI
- **WHEN** WORKBENCH_ADMIN_REDIRECT_URI is not set and an admin signs in from `https://cp.example.com/admin/`
- **THEN** the authorization request uses `https://cp.example.com/admin/` as its redirect URI

### Requirement: Deployment Setup Report
At startup, the control plane SHALL log where the admin console is served, or a warning that its build is missing. It SHALL log one warning, naming the setting and its effect, for each missing setting among OIDC_CLIENT_ID, WORKBENCH_PUBLIC_BASE_URL, WORKBENCH_BOOTSTRAP_ADMINS, WORKBENCH_SECRET_KEY and an LLM upstream. A warning MUST NOT stop the control plane.

#### Scenario: Only the guarded settings
- **WHEN** the control plane starts in production with only OIDC_DISCOVERY_URL and WORKBENCH_TELEMETRY_SECRET set
- **THEN** it starts, logs the console URL, and logs warnings naming OIDC_CLIENT_ID, WORKBENCH_PUBLIC_BASE_URL, WORKBENCH_BOOTSTRAP_ADMINS, WORKBENCH_SECRET_KEY and OPENAI_BASE_URL

#### Scenario: Complete setup
- **WHEN** every checked setting is present
- **THEN** the startup log contains the console URL and no setup warnings

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
