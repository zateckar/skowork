# Spec Delta

## Purpose

Let users review suggestions drawn from eligible changes in mail and calendar without granting the source authority to act.

## ADDED Requirements

### Requirement: Triage Is Opt In
The client SHALL evaluate mail or calendar events for triage only after the user enables triage and selects the eligible source scope. Triage SHALL be disabled by default.

#### Scenario: First use
- **WHEN** a user has not enabled triage
- **THEN** no source event is evaluated for triage.

### Requirement: Eligible Source Events
The client SHALL apply client/mail-calendar access, sensitivity and exclusion rules before accepting an event for triage, and SHALL stop evaluating a source when its access or selection is revoked.

#### Scenario: Excluded item
- **WHEN** a mail item is excluded by source policy
- **THEN** its content is not sent for triage and no suggestion is created.

### Requirement: Durable Event Identity
Triage SHALL identify source events and revisions durably and create at most one suggestion for each item revision despite duplicate delivery or interrupted evaluation. An edit or deletion SHALL invalidate an older in-flight evaluation and update or withdraw any outstanding suggestion.

#### Scenario: Replayed and deleted item
- **WHEN** a source event is redelivered after restart and the source item is subsequently deleted
- **THEN** the replay creates no duplicate suggestion and the outstanding suggestion is withdrawn.

#### Scenario: Delete races classification
- **WHEN** an item is deleted while a classification request for its earlier revision is in flight
- **THEN** its result cannot create or reactivate a suggestion.

#### Scenario: Ambiguous revisions in one source page
- **WHEN** a source page contains conflicting observations for the same item and their order cannot establish a current revision
- **THEN** the client does not advance the page cursor or guess which observation is current.

### Requirement: Triage Intake Overflow
When triage reaches its storage limit, the client SHALL allow source sync to continue, pause triage for the affected scope, and show that suggestions may have been missed. It SHALL resume only after the user acknowledges the gap and a fresh baseline is established.

#### Scenario: Queue full
- **WHEN** more eligible changes arrive than the pending triage limit permits
- **THEN** source sync continues, triage pauses with a visible gap, and no resumed triage is presented as complete until the user acknowledges the gap.

### Requirement: Triage Baseline
The client SHALL establish a baseline before evaluating newly enabled source scope and show when that baseline completed. Items included in the baseline SHALL NOT create historical suggestions.

#### Scenario: Enable triage
- **WHEN** the user enables triage for a mailbox that already contains messages
- **THEN** those baseline messages do not create suggestions and the monitoring start time is visible.

### Requirement: Bounded Relevance Evaluation
Triage SHALL evaluate eligible events against user-provided responsibilities using the organization model through the LLM gateway, with bounded batches and existing background deferral and model budgets. A failure SHALL leave events eligible for a later retry without producing a suggestion from an incomplete result.

#### Scenario: Gateway unavailable
- **WHEN** the gateway is unavailable during evaluation
- **THEN** the events remain pending and no suggestion is inferred from the failed request.

### Requirement: Reviewable Suggestions
Triage SHALL create at most one outstanding suggestion per eligible source item, with its source reference and a short reason, and SHALL allow the user to dismiss it or start work explicitly. A suggestion MUST NOT itself start a Run or execute a Tool.

#### Scenario: Relevant message
- **WHEN** an eligible message is evaluated as relevant
- **THEN** the user can review its reason and source and decide whether to start work.

### Requirement: Untrusted Event Content
Triage SHALL preserve source provenance and mark source content as untrusted for any Run started from a suggestion, as owned by client/agent-runtime; Tool decisions remain owned by client/policy-enforcement.

#### Scenario: Embedded instruction
- **WHEN** a message asks the agent to approve and send a reply
- **THEN** that request grants no authority and any later Tool call receives its normal policy decision.

### Requirement: Triage Retention And Disablement
Triage SHALL let the user disable triage and delete its pending events and suggestions; disabling SHALL stop new requests and discard results from in-flight requests. Deletion SHALL NOT delete the source item.

#### Scenario: Disable and delete
- **WHEN** the user disables triage and deletes its stored triage data
- **THEN** pending triage data and suggestions are removed while source mail remains unchanged.

### Requirement: Bounded Pending Triage
The client SHALL retain at most 1,000 pending triage events and expire events after 30 days, with a visible count of events that expired without evaluation.

#### Scenario: Gateway unavailable for 30 days
- **WHEN** a pending event reaches 30 days without a successful evaluation
- **THEN** it expires and the user can see that an event was not evaluated.

### Requirement: Revoked Source Content
Triage SHALL withdraw a suggestion and remove source-derived previews and reasons when source access is revoked or an item is excluded, including after a selection or policy change. It SHALL respect the deletion cascade of client/local-storage.

#### Scenario: Folder excluded after suggestion
- **WHEN** a suggested mail item's folder becomes excluded
- **THEN** the suggestion is withdrawn and its source-derived content is no longer shown or sent to the model.
