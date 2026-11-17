# Spec Delta

## ADDED Requirements

### Requirement: Effect-Aware Continuation
For a shutdown-failed Run whose recorded boundary is sufficient, the runtime SHALL offer explicit continuation without re-executing completed Tool calls. The failed Run and its history SHALL remain visible and the continuation SHALL be linked to it. Otherwise it SHALL explain that continuation is unavailable.

#### Scenario: Completed call before shutdown
- **WHEN** a Run fails at shutdown after a Tool result was recorded and the user chooses continuation
- **THEN** the continuation uses the recorded result and does not execute that call again.

#### Scenario: Insufficient history
- **WHEN** a legacy failed Run lacks a recoverable boundary
- **THEN** continuation is unavailable and no Tool is executed.

### Requirement: Uncertain Tool Outcomes
The runtime SHALL treat a Tool call with an unconfirmed effect as outcome unknown unless the Tool can safely reconcile it, including after a timeout, failure or shutdown. The runtime MUST NOT automatically repeat that call, and SHALL present the uncertainty for user resolution.

#### Scenario: Send accepted before crash
- **WHEN** a send may have succeeded but its result was not recorded before shutdown and the provider cannot reconcile it
- **THEN** continuation stops for user resolution and no automatic resend occurs.

#### Scenario: Connector response is lost
- **WHEN** a connector request may have reached its destination but the response is unavailable
- **THEN** the result and user-visible explanation state that the outcome is unknown and do not claim the call was not made.

### Requirement: Unknown Effect Guard
While an effect remains unknown, the runtime MUST block each new potentially effectful Tool call in the continuation until the user explicitly authorizes that call. The call SHALL also receive its normal current policy decision and any required Approval.

#### Scenario: Model reissues uncertain send
- **WHEN** a continuation requests the same send after the user chose to continue with its outcome unknown
- **THEN** it is blocked until the user explicitly authorizes a new attempt and the Tool passes its normal policy decision.

### Requirement: Atomic Continuation Claim
The runtime SHALL accept at most one continuation for a failed Run and preserve that claim and any pending resolution across restarts. Cancellation or revocation SHALL prevent further continuation work.

#### Scenario: Duplicate continuation
- **WHEN** the user activates continuation twice or the client restarts during creation
- **THEN** at most one continuation exists and no Tool is dispatched twice.

### Requirement: Continuation Authorization
The runtime SHALL apply current policy and the existing Approval and Question rules to every new Tool execution during continuation. An Approval SHALL authorize only the specific approved call and arguments.

#### Scenario: Policy changed during shutdown
- **WHEN** a continuation requests a new Tool call that is now denied
- **THEN** the call is denied even if the earlier Run had permission.

### Requirement: Child Run Progress Events
The runtime SHALL expose durable state changes and bounded current activity for each Child Run to its parent Thread, including waiting, completion, cancellation and failure, without expanding the Child Run's permitted Tools.

Run activity descriptors SHALL use fixed, bounded phase codes rather than Tool names, arguments, Tool output, arbitrary activity text or model output.

#### Scenario: Child asks a question
- **WHEN** a Child Run is interrupted for a Question
- **THEN** the parent Thread identifies the waiting child and presents the existing Action item.

### Requirement: Continuation Cancellation
The runtime SHALL preserve cancellation across restart and MUST NOT offer automatic continuation of a Run explicitly stopped by the user or revoked.

#### Scenario: Stop before restart
- **WHEN** the user stops a Run before the client restarts
- **THEN** restart does not resume or retry that Run.
