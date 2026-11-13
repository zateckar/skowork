# Proposal

## Why

On 2026-11-10 a user asked the agent to create a skill. The agent did not know where Skowork keeps skill files, had no tool to save one, and asked the user for a folder. Users cannot be expected to know where Skowork stores skills.

A skill the agent writes must become available to the user at once, and the user must be able to share it with colleagues. Today saving and sharing exist only in the skill editor in Settings and in "Save as skill" on a Thread (client/skills). The agent has no way to use them.

## What Changes

- **Save tool.** The agent gets the tool `skills.save`. It saves a Skill the agent drafts as one of the user's private Skills. The agent gives the name, description, instructions, optional declared fields and optional text files. It never gives a file path.
- **Validation.** A save is checked with the rules of the Skill editor and the registry's secret scan. Name collisions with Skills of other tiers, and with private Skills the agent did not create, are refused.
- **Approval.** The tool declares risk write-local, so policy.tools.risk decides it (approve by default). The approval shows the full text, the files, the declared fields, any personal-data findings and, for an update, the changes.
- **Update.** The agent may update a private Skill it created itself. It cannot change a Skill the user wrote in the editor.
- **Availability.** A saved Skill is listed to the agent from the next Run, in any Thread. A Run that is already executing keeps the Skills it started with. An explicit `/name` works at once.
- **Sharing.** After a save the agent may offer to share the Skill. It never shares. The user shares through the existing Publish dialog of client/skills, and confirms there.
- **Agent instructions.** The tool description and one fixed system prompt sentence tell the agent that no file path is needed and that it must not ask where to save.

Out of scope:
- Deleting a Skill through the agent. Deletion stays in the editor.
- A share tool for the agent, and sharing without the user's own confirmation.
- Binary resource files and files larger than the editor allows.
- Saving Skills from Unattended runs or Child Runs.
- Restoring earlier texts of an agent-updated Skill. The user can edit the Skill back in the editor.
- Changes to the Skill editor, the registry, the Publish dialog or the approval presentation. They are owned by their capabilities and only referenced here.

## Capabilities

### New Capabilities
- `client/agent-skills`: the agent's save and update tool for private Skills, its validation and approval, the availability of saved Skills, and the hand-off to sharing.

### Modified Capabilities
- None.

## Impact

- **Roadmap:** 41 [Managed]. The config.yaml roadmap has no category named [Feature]; its brackets name phases, and this change extends the Skills work of change 9, which is [Managed]. The roadmap ends at 38 in config.yaml, so number 41 leaves a gap that the product owner must confirm.
- **Depends on:**
  - add-skills (change 9, in flight at 20/23 tasks): the Skill editor, its validation, `save_private`, the catalogue reload, the Publish dialog and the Skill Tool Scope and Risk rules. This change cannot be applied before add-skills is archived.
  - add-agent-runtime (change 2): the core tool set, `find_tools`, approvals and the system prompt.
  - add-policy-and-sandbox (change 3): policy.tools.risk and policy.unattended.allowed_risk.
  - add-work-inbox (change 4): presentation of approvals.
  - fix-long-thread-model-calls (change 37, in flight): the stable prompt prefix that the availability rule depends on.
- **Code:**
  - `client/src/tools/`: the `skills.save` tool;
  - `client/src/runtime/`: the offer rules and the system prompt sentence;
  - `client/src/skills/`: the save path reused from the editor;
  - `client/tests/skills/` and `client/tests/runtime/`: new tests.
- **Data:** one new local table that records which private Skills the agent created. It holds no Skill text.
- **Policy keys:** none new. The tool is controlled by `policy.tools.rules["skills.save"]` and `policy.tools.risk.write-local`.
- **Control plane:** none.
