# client/work-inbox Specification

## Purpose
How agent work reaches the user: Threads and their feed, the Inbox views, and how approvals, questions and results are presented.

## Requirements

### Requirement: Images In The Thread Feed
The Thread feed SHALL show image attachments that a tool stored, such as images from image.generate, inline at the point of the tool call, scaled to the feed width. The user can open an image at full size and save it to a location they choose.

#### Scenario: Generated image appears
- **WHEN** image.generate stores an image during a Run
- **THEN** the feed shows the image under that tool call's card without the user expanding the card

#### Scenario: Save an image
- **WHEN** the user chooses Save as on an image in the feed
- **THEN** the client asks for a location and writes the image there

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

### Requirement: Generated Thread Surfaces
The Thread feed SHALL render supported, complete generated surfaces embedded in assistant messages, alongside their text, using native components. Surfaces SHALL support composable sections, cards, read-only text, tables, bar and line charts, and directed relationship diagrams.

#### Scenario: Visual comparison
- **WHEN** an assistant message contains a complete supported surface with a table and a bar chart
- **THEN** the Thread shows both visualizations at that point in the answer with the surrounding text

#### Scenario: Non-assistant content
- **WHEN** a user message, Chat message or tool result contains surface syntax
- **THEN** the feed treats it as content and does not activate a surface

### Requirement: Local Surface Exploration
The Thread feed SHALL let the user sort and filter a generated table and expand or collapse generated sections locally, without sending the interaction to the model.

#### Scenario: Sort without a model request
- **WHEN** the user sorts a generated table by a column
- **THEN** the rows are reordered in the Thread without starting or changing a Run

### Requirement: Generated Feedback Controls
Generated surfaces SHALL offer labeled buttons, single and multiple selections, switches, text fields and sliders, and SHALL submit their user-entered values only on an explicit user action. Submissions SHALL visibly identify the selections being sent.

#### Scenario: Adjust before submitting
- **WHEN** the user changes a switch and edits a text field
- **THEN** the values change locally and no response is sent until the user submits

### Requirement: Generated Surface Fallback
The Thread feed SHALL keep surrounding answer text readable and provide a readable fallback for an incomplete, invalid, oversized or unsupported generated surface. Such a surface MUST NOT expose active generated controls.

#### Scenario: Interrupted stream
- **WHEN** a response stops before a generated surface is complete
- **THEN** its text remains available as fallback and no controls from the incomplete surface can be activated

#### Scenario: Open a Thread after downgrade
- **WHEN** a user opens previously stored Threads with an older client after generated surfaces have been used
- **THEN** the older client can open the existing local data and display the original answer as readable text or code

### Requirement: Generated Surface Setting
The client SHALL provide a disabled-by-default setting for generated surfaces. Changes SHALL apply on the next client restart, and the setting SHALL explain this to the user. When disabled, the client SHALL retain the original readable response and SHALL NOT activate generated controls or request generated surfaces.

#### Scenario: Enable generated surfaces
- **WHEN** the user enables generated surfaces in Settings and restarts the client
- **THEN** eligible responses can display generated surfaces and the runtime can request them

#### Scenario: Disable generated surfaces
- **WHEN** the user disables generated surfaces and restarts the client
- **THEN** existing responses remain readable as text and their generated controls are inactive

### Requirement: Accessible Generated Surfaces
Generated surfaces SHALL support keyboard and assistive technology access, the client's themes and localization, and a text or table alternative for each chart and diagram. Chart alternatives SHALL include values, units and labels supplied by the surface.

#### Scenario: Chart without visual access
- **WHEN** a screen-reader user encounters a generated chart
- **THEN** its accessible alternative exposes its title, values, labels and supplied units

### Requirement: Generated Surface Evidence
Generated surfaces SHALL expose valid Citations associated with displayed values or content through the Thread's existing Citation interaction, and SHALL NOT present an unknown Citation identifier as a verified source.

#### Scenario: Referenced table value
- **WHEN** a generated table cell references an existing Citation
- **THEN** the user can open that Citation from the cell's displayed context

### Requirement: Generated Surface State
The Thread feed SHALL retain unsent input and local exploration state for a generated surface across streaming updates, navigation away and client restart. A submitted, superseded or unavailable surface SHALL show its status and prevent duplicate submission.

#### Scenario: Resume an unfinished response
- **WHEN** the user changes a form value, leaves the Thread and returns after restarting the client
- **THEN** the unfinished value is restored and has not been submitted

#### Scenario: Delete after downgrade
- **WHEN** a Thread containing unsent generated-surface data is deleted by an older client
- **THEN** its generated-surface data is deleted with the Thread and is not exposed as an ordinary attachment

#### Scenario: Duplicate activation
- **WHEN** a user activates the same submit control twice for the same surface revision
- **THEN** at most one submission is accepted

### Requirement: Generated Surface Limits
The client SHALL enforce finite limits on generated surface size, nesting, control count and data points before rendering, and SHALL not execute generated code or fetch network resources described by a surface.

#### Scenario: Oversized surface
- **WHEN** a generated surface exceeds a supported limit
- **THEN** the client shows its fallback without executing it or fetching external content
