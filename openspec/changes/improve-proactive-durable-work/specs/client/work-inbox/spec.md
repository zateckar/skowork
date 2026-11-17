# Spec Delta

## ADDED Requirements

### Requirement: Triage Suggestion Presentation
The Inbox SHALL show each triage suggestion in a Thread with its source, reason and current status, and provide accessible actions to dismiss it or explicitly start work. A withdrawn suggestion SHALL identify the withdrawal while hiding source-derived content as required by client/event-triage.

#### Scenario: Source deleted
- **WHEN** a source item is deleted after its suggestion appears
- **THEN** the Thread shows that the suggestion was withdrawn and cannot start work on it.

### Requirement: Background Run Visibility
The Thread SHALL show the current state and most recent bounded activity of its Run and Child Runs, including waiting, completion, failure and cancellation, and identify pending Action items using existing Action views.
Running and Action list entries SHALL expose the current top-level Run's fixed activity phase when available, alongside the existing Action status, to both visual and accessibility representations. A non-running Run or an unrecognized phase SHALL use the existing generic status.

#### Scenario: Parallel children
- **WHEN** two Child Runs are active and one waits for a Question
- **THEN** the Thread distinguishes their states and links to the pending Question.

### Requirement: Background Work Controls
The Thread SHALL provide accessible Stop and existing message queueing and steering controls for eligible Runs, and show when a steering message is queued until the next safe boundary.

#### Scenario: Long Tool call
- **WHEN** the user sends steering while a Tool call is executing
- **THEN** the Thread indicates that the message is queued until the Run can receive it.

### Requirement: Recovery Choice Presentation
The Thread SHALL explain a shutdown-failed Run's continuation option and any unknown Tool outcome, and SHALL offer only the resolutions supported by the runtime without implying that an unknown effect succeeded or failed.

#### Scenario: Ambiguous send
- **WHEN** a send has an unknown outcome after restart
- **THEN** the Thread identifies the uncertainty and does not label the message sent or unsent.
