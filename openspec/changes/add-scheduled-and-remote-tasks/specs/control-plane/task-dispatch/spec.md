# Spec Delta

## Purpose

Defines how people and organization systems ask Skowork to perform work for a user or Group through the control plane, and how those Remote tasks are authorized, delivered, tracked and audited.

## ADDED Requirements

### Requirement: Task Request API
Task dispatch SHALL provide an authenticated API to create a Remote task. A request contains:
- a target user or Group;
- a prompt;
- an optional skill;
- optional attachments up to 10 MB;
- an expiry (default 24 hours, maximum 7 days);
- an optional callback URL.

The response contains a task id.

#### Scenario: Create task for a user
- **WHEN** an authorized issuer posts a Remote task for user "jnovak"
- **THEN** the API returns a task id with status queued

### Requirement: Request Content Is Untrusted
Task dispatch SHALL deliver a Remote task's prompt and attachments to the client labelled as untrusted content with the issuer as provenance. It MUST NOT attach any approval, policy or permission data to a task request.

#### Scenario: Request claims approval
- **WHEN** an issuer includes a field asserting that the user pre-approved all actions
- **THEN** the field is not delivered as data with any authority, and the client's decisions follow client/policy-enforcement only

### Requirement: Issuer Authentication
Task dispatch SHALL accept requests from people authenticated through control-plane/identity, and from service principals registered by administrators with their own credentials. Every task records its issuer.

#### Scenario: Unregistered system
- **WHEN** a request arrives with credentials that belong to no registered service principal
- **THEN** it is rejected with an authentication error and no task is created

### Requirement: Targeting Authorization
Task dispatch SHALL authorize each request against the dispatch rules defined in control-plane/policy-service. Those rules state which issuers may target which users or Groups, for example group owners targeting their Group, or registered systems targeting specific Groups. Unauthorized requests MUST be rejected.

#### Scenario: Colleague not permitted
- **WHEN** a user without a dispatch rule targeting "finance" submits a task for a finance user
- **THEN** the request is rejected with a not-authorized error and audited

### Requirement: Group Fan-Out
A task targeting a Group SHALL be expanded into one Remote task per member at request time, and each member's task is tracked separately under the parent task id.

#### Scenario: Group task
- **WHEN** a task targets a Group with 12 members
- **THEN** 12 member tasks are created under the parent id, each with its own status

### Requirement: Delivery Through Fleet Push
Task dispatch SHALL deliver each member task as a remote-task push through control-plane/fleet-registry to all of the target user's connected installations. Installations claim a task through client/control-channel, and only the first claim succeeds, so the task runs at most once per user. The others receive remote-task-claimed. Undelivered tasks are kept until they expire.

#### Scenario: User has two laptops
- **WHEN** a Remote task is delivered to a user with two connected installations
- **THEN** only the first installation to claim runs it, and the other receives remote-task-claimed

#### Scenario: Expired before delivery
- **WHEN** a task's expiry passes before any installation claims it
- **THEN** its status becomes expired, and the issuer can see that status

### Requirement: Client Task Messages
Task dispatch SHALL accept exactly these client→server messages for a member task, sent through client/control-channel:
- remote-task-claim: request to claim;
- remote-task-accept / remote-task-decline: the user's acceptance decision;
- remote-task-status: running, or a final state of succeeded, failed, dead or cancelled, with optional result content.
Messages from an installation that does not hold the claim MUST be refused, except remote-task-claim.

#### Scenario: Status from non-claiming installation
- **WHEN** an installation that did not win the claim sends remote-task-status
- **THEN** the message is refused and audited, and the task status is unchanged

### Requirement: Claim Refusal After Cancel Or Expiry
Task dispatch SHALL refuse a remote-task-claim for a task that is already claimed, cancelled or expired. On cancellation or expiry, it SHALL push remote-task-cancelled to every installation of the target user, not only the claiming one.

#### Scenario: Claim after cancellation
- **WHEN** an installation that was offline reconnects and claims a task the issuer already cancelled
- **THEN** the claim is refused, and the installation has already received remote-task-cancelled

### Requirement: Task Status Tracking
Task dispatch SHALL track each member task through these statuses: queued, delivered, claimed, awaiting-acceptance, accepted, running, then succeeded, failed, declined, expired or cancelled. A claimed task skips awaiting-acceptance when the user already trusts the issuer. A status query for the issuer SHALL be provided.

#### Scenario: Status mapping from client reports
- **WHEN** the claiming client reports remote-task-status dead, or sends remote-task-decline
- **THEN** the member task becomes failed, or declined respectively; client succeeded, failed and cancelled map to the statuses of the same name

#### Scenario: Issuer checks status
- **WHEN** the issuer queries a task id
- **THEN** it receives the current status of each member task and the times of each transition

### Requirement: Result Sharing
Task dispatch SHALL return a Run's result content to the issuer only when the target user's policy.scheduler.remote_share_results lists that issuer (issuer ids; default empty). Otherwise it returns status only. Results are delivered by status query, or by a callback to a URL on the callback allowlist. The allowlist is a control-plane server setting managed in control-plane/admin-console, not a policy key.

#### Scenario: Results not shared
- **WHEN** a task succeeds and the target's policy does not allow result sharing
- **THEN** the issuer sees status succeeded with no result content

### Requirement: Cancellation By Issuer
The issuer SHALL be able to cancel a task that is not yet finished. Cancellation is pushed as remote-task-cancelled to the target user's installations, and the claiming installation cancels the Run.

#### Scenario: Cancel running task
- **WHEN** the issuer cancels a running task
- **THEN** the installation cancels the Run, and the status becomes cancelled

### Requirement: Dispatch Audit And Limits
Task dispatch SHALL audit every request, authorization decision, delivery, acceptance and result disclosure. It SHALL enforce per-issuer rate limits, which administrators configure in control-plane/admin-console (default 100 tasks per hour).

#### Scenario: Rate limit exceeded
- **WHEN** a system issuer exceeds its hourly limit
- **THEN** further requests are rejected with a rate-limit error until the window resets, and the rejection is audited
