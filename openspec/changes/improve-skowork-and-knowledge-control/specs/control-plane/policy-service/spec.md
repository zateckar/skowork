# Spec Delta

## ADDED Requirements

### Requirement: Complete Policy Key Metadata
The policy service SHALL expose every supported configurable policy key with its owning capability, meaning, type, allowed values, default, merge behavior and guardrail behavior. Dynamic map entries and generated bundle fields SHALL be identified separately; a Tool id is a rule-map entry, not a new top-level policy key.

#### Scenario: Tool decision path
- **WHEN** an administrator requests metadata for computer.click
- **THEN** it identifies policy.tools.rules[computer.click] and policy.unattended.rules[computer.click], not an invented policy.computer.click key

#### Scenario: Generated MCP field
- **WHEN** the administrator browses policy.mcp.servers
- **THEN** it is identified as generated from control-plane/mcp-catalog and is not offered as an editable policy field

### Requirement: Complete Tool Decision Catalog
The service SHALL expose a versioned catalog of every supported built-in Tool, including tools disabled by policy, and authorized discovered MCP Tools. Each entry identifies its Tool id, description, owning capability, base Risk class, dynamic risk caveats, feature prerequisites and applicable decision maps. Custom valid rule patterns remain supported.

#### Scenario: Computer tools are discoverable
- **WHEN** computer use is disabled and an admin requests the catalog
- **THEN** computer.observe, computer.launch, computer.screenshot, computer.click, computer.type, computer.keys, computer.scroll, computer.drag and computer.select remain discoverable with their prerequisites

#### Scenario: MCP visibility
- **WHEN** an administrator requests discovered MCP Tools
- **THEN** the catalog includes only servers the administrator is authorized to inspect, with their current discovered Tool ids and risk metadata

### Requirement: Policy Coverage Verification
Release validation SHALL verify agreement between owning capability policy declarations, server validation metadata, client-supported policy keys, registered built-in Tool ids and the admin catalog. Missing or stale entries SHALL fail validation with the unmatched key or Tool id and its owner.

#### Scenario: New tool omitted from catalog
- **WHEN** a client build introduces a Tool without catalog metadata
- **THEN** validation fails naming that Tool before the release is published

#### Scenario: Setting omitted from client support
- **WHEN** an editable policy setting has no client support declaration or intentional server-only classification
- **THEN** validation fails instead of claiming complete policy coverage

### Requirement: Explain Effective Tool Decisions
Policy simulation SHALL explain a selected user's Tool decision for attended or Unattended execution using the semantics of client/policy-enforcement. The explanation includes matching patterns, risk fallback, Group precedence, guardrails, feature prerequisites and Tool-declared floors, and identifies facts requiring a concrete action or device state.

#### Scenario: Unattended click floor
- **WHEN** simulation evaluates an Unattended computer.click with no explicit unattended allowance
- **THEN** it reports the approval floor defined by client/computer-use rather than inferring permission from an attended allow rule

#### Scenario: Unknown device state
- **WHEN** simulation cannot know the target window's reachability or the actual action's dynamic risk
- **THEN** it states those conditions and does not promise that the client will execute the action
