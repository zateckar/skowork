# Spec Delta

## ADDED Requirements

### Requirement: Child Run Approval Expiry
An Approval raised by a child Run in an attended Run SHALL expire when it has been unanswered for 30 minutes. The runtime SHALL then resolve it as rejected, record the expiry in the Activity log, and return the tool result "The user did not answer in time, so the action did not run." to the child Run, which continues. This rule does not apply to Approvals of top-level Runs or to Unattended runs.

#### Scenario: Skill approval unanswered
- **WHEN** a Skill child Run requests approval for `mail.draft` in an attended Run, and the user does not answer for 30 minutes
- **THEN** the Approval is resolved as rejected, the Activity log records the expiry, and the Skill continues with the expiry text as the result of that call

The application checks expiry every 30 seconds, so an Approval is resolved within 30 seconds after its 30 minutes have passed.

#### Scenario: Top-level approval stays open
- **WHEN** a top-level Run is interrupted for approval and the user does not answer for 2 hours
- **THEN** the Approval stays open, as Approval Interrupts requires

### Requirement: Child Run Time Limit
A child Run in an attended Run SHALL end as failed with reason "subagent time limit reached" when 60 minutes have passed since it started, counting the time it spent waiting for the user. Before it ends, the runtime SHALL cancel the child's open Action items. The parent Run receives the failure as the result of the call that started the child. wall_minutes is unchanged.

#### Scenario: Question left open
- **WHEN** a subagent asks the user a question with `ask_user`, and nobody answers for 60 minutes
- **THEN** the question is cancelled, the subagent ends as failed with reason "subagent time limit reached", and the parent Run continues with that failure as the call's result

#### Scenario: Waiting counts toward the child limit
- **WHEN** a child Run waited 40 minutes for an answer and then runs for 25 more minutes
- **THEN** the child Run ends as failed at the 60-minute mark

#### Scenario: Child executing at the limit
- **WHEN** a child Run is executing a model or tool call when its 60 minutes pass
- **THEN** the call in progress finishes, and the child Run ends as failed at its next step
