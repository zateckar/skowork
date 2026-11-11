# Spec Delta

## Purpose

Defines how the agent saves and updates Skills the user asked it to create. A saved Skill is a private Skill of the user. It is usable from the next Run, and it reaches colleagues only when the user shares it through the Skill sharing of client/skills.

## ADDED Requirements

### Requirement: Save Skill Tool
The client SHALL provide the tool `skills.save`, which saves a Skill that the agent drafted as one of the user's private Skills. Its arguments are name, description and instructions, and optionally version, tools, risk, network and files. The tool MUST NOT accept a file path, and the client chooses where the Skill is stored.

#### Scenario: Save without a path
- **WHEN** the user asks the agent to create a Skill and the agent calls `skills.save` with name, description and instructions
- **THEN** the Skill is stored as a private Skill, and the agent is not asked for any location

### Requirement: Agent Told No Path Is Needed
The client SHALL tell the agent, in the description of `skills.save` and in the system prompt of each top-level Run that is not an Unattended run, that a Skill needs no file path, that the tool stores it, and that the agent MUST NOT ask the user where to save it. `find_tools` SHALL return `skills.save` for a request to create a Skill.

#### Scenario: Request to create a Skill
- **WHEN** the user asks the agent to make a reusable Skill from the current Thread
- **THEN** the agent finds `skills.save` through `find_tools` and does not ask the user for a folder or file name

### Requirement: Save Validation
A saved Skill SHALL pass the validation of the Skill editor in client/skills, with the same limits. Text that contains a secret MUST be refused. The secret rules are those of control-plane/skill-registry Automated Skill Scan; until that scan exists, the client applies the rules listed in design D8. A refusal SHALL name the field or file that failed, and MUST NOT show the secret value.

#### Scenario: Invalid frontmatter
- **WHEN** the agent saves a Skill whose name is not kebab-case
- **THEN** the save is refused with the error naming the name field, and nothing is stored

#### Scenario: Secret in instructions
- **WHEN** the agent saves a Skill whose instructions contain an API key
- **THEN** the save is refused, the result names the instructions field, and the key is not shown

### Requirement: Name Collisions
A save MUST be refused when its name is used by a usable Skill of another tier, or by a private Skill that the agent did not create. The refusal SHALL name the conflicting Skill and its tier, and the agent SHALL propose another name.

#### Scenario: Name of an org Skill
- **WHEN** the agent saves a private Skill with the name of a usable org Skill
- **THEN** the save is refused, and the result names the org Skill

#### Scenario: Name of a user's own Skill
- **WHEN** the agent saves a Skill with the name of a private Skill the user wrote in the Skill editor
- **THEN** the save is refused and the user's Skill is unchanged

### Requirement: Update Of Agent-Created Skills
The tool SHALL update a private Skill that the agent created only when the call sets update to true and the version is higher than the stored version. The tool MUST NOT change a private Skill that the user created in the Skill editor.

#### Scenario: Update with a higher version
- **WHEN** the agent saves a private Skill it created earlier with update set to true and version 1.1.0 over 1.0.0
- **THEN** the approval shows the changed lines, and after approval the stored Skill is replaced

#### Scenario: Update without a higher version
- **WHEN** the agent saves over its own Skill with an equal or lower version
- **THEN** the save is refused, and the stored Skill is unchanged

### Requirement: Previous Version Of An Updated Skill
When the tool updates an Agent-created Skill, the client SHALL keep the replaced version: its SKILL.md and resource files. Only the latest replaced version is kept. The user SHALL be able to restore it from the Skill editor; a restore makes the kept version current and keeps the replaced one in its place. Deleting the Skill deletes its kept version.

#### Scenario: Restore after an update
- **WHEN** the agent updated an Agent-created Skill from 1.0.0 to 1.1.0, and the user chooses Restore previous version in the Skill editor
- **THEN** version 1.0.0 is current again, and version 1.1.0 is kept as the previous version

### Requirement: Approval Before Save
A save and an update SHALL be decided by the risk class write-local of policy.tools.risk, which requires approval by default. The approval SHALL show the name, description, full instructions, declared tools, risk and network, the path and size of each file, the personal data findings of requirement Personal Data In Saved Skills and, for an update, the changed lines against the stored text.

#### Scenario: Approval shows the full text
- **WHEN** the agent calls `skills.save` and policy requires approval
- **THEN** the user sees the complete instructions and file list before the Skill is stored

### Requirement: Personal Data In Saved Skills
The client SHALL find personal data in a Skill to be saved with the patterns of client/skills Save Thread As Skill: email addresses, Windows and UNC paths, and person names from the Thread. It SHALL show each finding in the approval and MUST NOT replace it without the user's edit.

#### Scenario: Email address in instructions
- **WHEN** the instructions of a Skill to be saved contain an email address
- **THEN** the approval lists the address as a personal data finding, and the saved text still contains it only if the user approves it

### Requirement: Skill Availability After Save
A saved Skill SHALL be listed to the agent in every Run that starts after the save, in any Thread. A Run that is executing SHALL keep the Skills it started with. An explicit `/name` invocation after the save SHALL use the saved Skill at once.

#### Scenario: Next Run uses the Skill
- **WHEN** the agent saves a Skill in one Run and the user starts a new Run in another Thread
- **THEN** the new Run lists the Skill to the agent

#### Scenario: Running Run keeps its list
- **WHEN** a Skill is saved during a Run
- **THEN** that Run does not list the new Skill to the model, and the next Run does

### Requirement: Attended Top-Level Runs Only
The tool SHALL be offered only in top-level Runs that are not Unattended runs. A call from an Unattended run or from a Child Run SHALL return an error and change nothing.

#### Scenario: Trigger-started Run
- **WHEN** a Trigger-started Run calls `skills.save`
- **THEN** the call returns an error, and no Skill is stored

### Requirement: Resource Files Of Saved Skills
The files passed to the tool SHALL be text. Each SHALL have a safe relative path under scripts/, references/ or assets/, within the file limits of client/skills. Binary content MUST be refused.

#### Scenario: Binary file
- **WHEN** the agent passes a file whose content is not text
- **THEN** the save is refused, and nothing is stored

#### Scenario: Unsafe path
- **WHEN** the agent passes a file whose path leaves the Skill folder
- **THEN** the save is refused

### Requirement: Agent-Created Skill Record
The client SHALL record each private Skill the agent creates, with its creation time, its last update time and the Thread that created it. The record MUST NOT contain Skill text. Deleting that Skill in the Skill editor SHALL remove the record.

#### Scenario: Deleting an agent-created Skill
- **WHEN** the user deletes an agent-created Skill in the Skill editor
- **THEN** its record is removed, and the agent can no longer update that name

### Requirement: Save Result
The tool result SHALL name the Skill, state that it is private, state whether it was created or updated, and state that it is usable from the next Run.

#### Scenario: Created result
- **WHEN** a save succeeds
- **THEN** the result states the Skill's name, that it is private and created, and that it is usable from the next Run

### Requirement: Saved Skill Effect Limits
A saved Skill SHALL take effect only within its user's policy. Its declared tools, risk and network follow client/skills Skill Tool Scope, Skill Risk Declaration and Skill Network Declaration. The save MUST NOT grant any tool or approval.

#### Scenario: Skill declares a disallowed tool
- **WHEN** the agent saves a Skill that declares shell.run and the user's policy does not allow it
- **THEN** the Skill is saved, and when it runs, shell.run is not available to it

### Requirement: Sharing Stays With The User
After a save, the agent MAY offer to share the Skill. The agent MUST NOT share a Skill. The saved Skill SHALL be named in its Thread with a link to its Publish dialog, and sharing happens only there, after the user confirms the scope and audience.

#### Scenario: Agent offers to share
- **WHEN** the agent has saved a Skill and the user asks for it to be shared with a colleague
- **THEN** the agent does not share it, and the Thread offers the Publish dialog of the Skill for the user to confirm

#### Scenario: Share without confirmation
- **WHEN** the agent tries to share a Skill through any tool
- **THEN** no such tool is offered to it, and no Skill is shared
