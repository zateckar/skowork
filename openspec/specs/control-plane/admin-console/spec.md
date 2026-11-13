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
The control plane SHALL serve the admin console at `/admin/`. When the console is built, a request for the root path `/` SHALL be redirected to `/admin/` with its query string unchanged, so a sign-in response sent to the root completes the sign-in. When SKOWORK_ADMIN_REDIRECT_URI is not set, the console SHALL use `<console origin>/admin/` as its sign-in redirect URI.

#### Scenario: Open the root path
- **WHEN** an admin opens `https://<control plane>/` in a browser
- **THEN** the browser is redirected to `https://<control plane>/admin/` and the console loads

#### Scenario: Sign-in response at the root
- **WHEN** the redirect URI is `https://<control plane>` and the identity provider sends the browser to `https://<control plane>/?state=s&code=c`
- **THEN** the browser is redirected to `https://<control plane>/admin/?state=s&code=c`, and the console exchanges the code for a token

#### Scenario: Default redirect URI
- **WHEN** SKOWORK_ADMIN_REDIRECT_URI is not set and an admin signs in from `https://cp.example.com/admin/`
- **THEN** the authorization request uses `https://cp.example.com/admin/` as its redirect URI

### Requirement: Deployment Setup Report
At startup, the control plane SHALL log where the admin console is served, or a warning that its build is missing. It SHALL log one warning, naming the setting and its effect, for each missing setting among OIDC_CLIENT_ID, SKOWORK_PUBLIC_BASE_URL, SKOWORK_BOOTSTRAP_ADMINS, SKOWORK_SECRET_KEY and an LLM upstream. A warning MUST NOT stop the control plane.

#### Scenario: Only the guarded settings
- **WHEN** the control plane starts in production with only OIDC_DISCOVERY_URL and SKOWORK_TELEMETRY_SECRET set
- **THEN** it starts, logs the console URL, and logs warnings naming OIDC_CLIENT_ID, SKOWORK_PUBLIC_BASE_URL, SKOWORK_BOOTSTRAP_ADMINS, SKOWORK_SECRET_KEY and OPENAI_BASE_URL

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

### Requirement: Notice Lifetime
A success notice (Action Feedback) SHALL go away by itself after a few seconds; an error notice stays until it is dismissed. A notice belongs to the view it was raised in and is cleared when the admin opens another view.

#### Scenario: Notice does not follow the admin
- **WHEN** an admin grants a role in the Identity view and then opens the Policies view
- **THEN** the notice about the grant is no longer shown

### Requirement: Button Roles
The admin console SHALL draw buttons by their role: the main constructive action of a form as the primary button, actions that revoke, deny, lift, retire, remove or delete in the danger style, and every other action as a neutral button.

#### Scenario: Deny button
- **WHEN** an admin opens the deny list in the Identity view
- **THEN** the Deny button is drawn in the danger style

### Requirement: Skill Review Workflow
The Skill review view SHALL open the review of a queued version in a dialog titled with the Skill and version, let the reviewer close the review without deciding, show Approve as the primary action, and list every Skill version, whatever its state, under All Skill versions with a state label.

#### Scenario: Open and close a review
- **WHEN** a reviewer opens the review of a queued version and then closes it
- **THEN** the review is shown in a dialog named after the version while it is open, and the version stays in the queue undecided when it is closed

### Requirement: Own Version Guidance
Before a reviewer decides on their own Skill version, the review SHALL say whether they can approve it, as control-plane/skill-registry Org Review And Signing defines: as the sole reviewer with a required justifying comment, or not at all because another reviewer must.

#### Scenario: Sole reviewer opens own import
- **WHEN** the only platform-admin opens the review of a Skill they imported
- **THEN** the review says that their approval will be recorded as a self-approval, and that a comment is required to approve

#### Scenario: Another reviewer exists
- **WHEN** an author opens the review of their own version while another skill-reviewer exists
- **THEN** the review says that another reviewer must approve it, and the Approve button is disabled

### Requirement: Records First Views
Each admin console view SHALL show its records as a table, with a toolbar that holds a filter for the records, their count and the action that adds a record. Forms for adding or editing records MUST NOT be shown on the page next to the records. An empty table says what the records are and offers the add action.

#### Scenario: Identity view without forms
- **WHEN** a platform-admin opens the Identity view
- **THEN** the view shows the administrator roles as a table with an action to grant a role, and no input field is shown until that action is chosen

#### Scenario: Filter records
- **WHEN** an admin types "ali" into the filter of the Group members table
- **THEN** only the records that contain "ali" are shown, and the count says how many of all records match

### Requirement: Record Dialogs
Adding a record and editing a record SHALL open a modal dialog with the record's form. The dialog has a title that names the action, moves focus to its first field, keeps focus inside, closes on Escape or Cancel without changes, and shows a failed save's error inside the dialog with the entered values kept. A successful save closes the dialog and shows the outcome as a notice (Action Feedback).

#### Scenario: Failed save keeps the values
- **WHEN** an admin adds an LLM connection whose URL the server rejects
- **THEN** the dialog stays open with the error and the entered values, and the connection list is unchanged

#### Scenario: Edit an MCP server
- **WHEN** an admin chooses Edit for an MCP catalog entry, changes its Groups and saves
- **THEN** the dialog closes, the table shows the new Groups, and a notice confirms the save

### Requirement: Pickers For Known Values
A form field that names a user, a Group, an identity-provider role, an administrator role, a service principal or a catalog entry SHALL offer the values the control plane knows as a searchable list. A field that takes several values shows them as removable chips. Where the control plane accepts a value it does not know yet, such as a new Group or a user who has not signed in, the field also accepts typed text and marks it as new.

#### Scenario: Owners of a new Group
- **WHEN** a platform-admin defines a Group and types "ali" into its owners field
- **THEN** the field lists the known users that match, and choosing alice adds her as a chip

#### Scenario: Unknown user
- **WHEN** an admin grants a role to a user name nobody has signed in with
- **THEN** the field accepts it and marks it as a user the control plane has not seen

### Requirement: Confirmation Dialogs
Destructive actions SHALL ask for confirmation in a dialog that names the record and the consequence, with the confirming button labelled by the action. Where the action records a reason, the dialog asks for it. Cancelling changes nothing.

#### Scenario: Revoke an installation
- **WHEN** an admin chooses Revoke for an installation
- **THEN** a dialog names the user and machine, explains that the user is signed out there, and asks for a reason; nothing is revoked if the admin cancels

### Requirement: View Sections
A view with several independent record sets SHALL show them as tabs, one record set at a time, with the tab in the view's address so that it survives a reload. The Identity view has the tabs Administrators, Group members, Role mappings, Service principals and Deny list; the Remote tasks view has Dispatch rules, Rate limits and Callbacks.

#### Scenario: Reload keeps the tab
- **WHEN** an admin opens the Deny list tab of the Identity view and reloads the page
- **THEN** the Deny list tab is shown
