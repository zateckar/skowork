# Spec Delta

## MODIFIED Requirements

### Requirement: Structured Policy Editor
The admin console SHALL let an admin edit a policy document through typed controls generated from the policy schema of control-plane/policy-service, grouped by top-level key, showing each key's owner, meaning, allowed values and default. Unset keys are labeled inherited without confusing the schema default with an effective value. The raw JSON view edits the same draft and validation errors appear beside the affected control. Tool decisions are discoverable under Complete Policy Discovery.

#### Scenario: Enable computer use without JSON
- **WHEN** an admin switches computer use to allow for a Group, saves its draft and publishes
- **THEN** the published document contains policy.tools.computer.mode set to allow and the JSON view shows the same document

#### Scenario: Error next to the control
- **WHEN** an admin enters an invalid host pattern and saves
- **THEN** the validation error appears next to the list and nothing is published

## ADDED Requirements

### Requirement: Skowork Console Identity
The console SHALL identify the product as Skowork in its page title, sign-in, navigation, dashboard descriptions and visible product guidance, while preserving existing authentication and deployment configuration compatibility.

#### Scenario: Open the console
- **WHEN** an administrator opens the console and signs in
- **THEN** the browser title, sign-in page and console navigation say Skowork

### Requirement: Complete Policy Discovery
The policy editor SHALL search setting paths, descriptions, Tool ids and rule patterns, including unset and disabled features. Tool results expose attended and unattended decisions through policy.tools.rules and policy.unattended.rules, risk defaults and feature prerequisites, using the catalog of control-plane/policy-service.

#### Scenario: Find computer.click
- **WHEN** an administrator searches for computer.click with no rule set
- **THEN** the editor finds the Tool, offers an explicit allow, approve or deny rule in either run mode, and links its risk defaults and policy.tools.computer.mode prerequisite

#### Scenario: Find a new policy
- **WHEN** the server schema adds policy.speech.download
- **THEN** the editor exposes its typed control, default and description without a manually maintained section hiding it

### Requirement: Tool Rule Explanation
For a selected user and run mode, the editor SHALL show the effective decision explanation supplied by control-plane/policy-service, including matching rules, Group precedence, guardrails, feature gates and risk floors. Without a selected user it SHALL identify a draft preview and MUST NOT claim to show that user's effective decision.

#### Scenario: Allow rule but feature disabled
- **WHEN** a draft allows computer.click while policy.tools.computer.mode remains off
- **THEN** the editor explains that the Tool remains unavailable and offers navigation to the feature setting

#### Scenario: Destructive action has a higher floor
- **WHEN** the effective external risk is allowed but a click is classified destructive
- **THEN** the explanation distinguishes the Tool's base risk from the actual action's destructive policy result

### Requirement: Tool Rule Picker
Adding or editing a Tool rule SHALL use a searchable picker of known built-in and authorized MCP Tools and common patterns, while accepting valid custom patterns. Reset removes the explicit rule to restore inheritance. Existing custom rules MUST survive editing and refresh, and no selection grants allow implicitly.

#### Scenario: Set a click approval
- **WHEN** the admin chooses computer.click, explicitly chooses approve and saves the rule
- **THEN** the draft contains policy.tools.rules[computer.click] equal to approve, with no unrelated policy changes

#### Scenario: Future MCP rule
- **WHEN** a draft contains a valid rule for an MCP Tool not currently discovered
- **THEN** the editor preserves it and labels it as not currently known instead of deleting it

### Requirement: Inspectable Shared Publications
The Knowledge administration view SHALL list shared items and category publications as records with owner, audience, version, state and last update. A record opens its exact published text and permitted evidence, and removal uses a dialog with its effect and reason under control-plane/team-knowledge. Private unpublished Source data MUST NOT be accessible from this view.

#### Scenario: Inspect a category publication
- **WHEN** a platform-admin opens a published Project Falcon record
- **THEN** they see its published entries, audience and version, and can remove it with a reason without accessing the owner's unpublished knowledge
