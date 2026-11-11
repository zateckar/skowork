# Spec Delta

## ADDED Requirements

### Requirement: Child Run Action Items
The feed of a Thread SHALL show the open Approval and Question cards of every interrupted child Run directly after the tool call that started it, under that subagent's entry. Each card offers the same actions as a top-level card, and answering it resumes the child Run and its parent Run. While such an Action item is open, the Thread is listed in the Action view.

#### Scenario: Skill needs approval
- **WHEN** the Skill "internal-policy-lookup", started by `use_skill`, asks to run a tool that needs approval
- **THEN** the Thread feed shows an Approval card directly after that `use_skill` call, and approving it lets the Skill finish and the Run continue

#### Scenario: Child Question answered in the feed
- **WHEN** a subagent asks a Question with fixed choices
- **THEN** the feed shows a Question card directly after the call that started the subagent, and the choice the user picks is returned to the subagent
