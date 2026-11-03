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
