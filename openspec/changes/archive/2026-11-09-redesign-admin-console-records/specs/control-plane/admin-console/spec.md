# Spec Delta

## ADDED Requirements

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

### Requirement: Notice Lifetime
A success notice (Action Feedback) SHALL go away by itself after a few seconds; an error notice stays until it is dismissed. A notice belongs to the view it was raised in and is cleared when the admin opens another view.

#### Scenario: Notice does not follow the admin
- **WHEN** an admin grants a role in the Identity view and then opens the Policies view
- **THEN** the notice about the grant is no longer shown

## MODIFIED Requirements

### Requirement: Skill Review Workflow
The Skill review view SHALL open the review of a queued version in a dialog titled with the Skill and version, let the reviewer close the review without deciding, show Approve as the primary action, and list every Skill version, whatever its state, under All Skill versions with a state label.

#### Scenario: Open and close a review
- **WHEN** a reviewer opens the review of a queued version and then closes it
- **THEN** the review is shown in a dialog named after the version while it is open, and the version stays in the queue undecided when it is closed