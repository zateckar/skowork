# Spec Delta

## MODIFIED Requirements

### Requirement: Run Lifecycle States
Every Run SHALL be in exactly one of these states: queued, running, interrupted, succeeded, failed or cancelled. Only these transitions are allowed:
- queued to running, failed or cancelled;
- running to interrupted, succeeded, failed or cancelled;
- interrupted to running or cancelled;
- failed to queued, only through Run Retry.
Every state change MUST be persisted before it is announced to the UI.

#### Scenario: Successful run
- **WHEN** a user message starts a Run and the model finishes without further tool calls
- **THEN** the Run moves queued → running → succeeded and each transition is stored with a timestamp

#### Scenario: Restart during a run
- **WHEN** the client process exits while a Run is running
- **THEN** after restart that Run is marked failed with reason "interrupted by shutdown", and the Thread offers to retry it

#### Scenario: Restart with queued runs
- **WHEN** the client process exits while Runs are queued
- **THEN** after restart those Runs are still queued, in their original order, and start when slots are free

#### Scenario: Queued run cancelled or revoked
- **WHEN** the user stops a queued Run, or the client receives a revocation while Runs are queued
- **THEN** a stopped Run moves from queued to cancelled, and on revocation every queued Run moves to failed with reason "access revoked", without ever running

### Requirement: Run Context
The runtime SHALL give every Run and subagent Run the context the model cannot discover with tools:
- in the system prompt: the UTC offset and the Windows time zone, the signed-in user's name, and the user's home, Documents, Downloads and Desktop folders;
- on the first line of the message that starts the Run: the local date, weekday and time.

The time line is stored with that message and is resent unchanged whenever the message is part of a later request.

#### Scenario: Relative date
- **WHEN** the user asks "What is on my calendar tomorrow?" on Thursday 22 October 2026
- **THEN** the message the model receives starts with a line stating that it is Thursday 22 October 2026, so the model can ask for 23 October without calling a tool to learn the date

#### Scenario: Earlier Turn keeps its time
- **WHEN** a user message sent at 09:12 is resent as part of the conversation of a Run that starts at 15:40
- **THEN** it still carries the 09:12 time line, and only the new message carries 15:40

### Requirement: Run Context Stability
The runtime SHALL fix the Run context when a Run starts executing, so it does not change between the model calls of that execution. A Run that resumes after an interrupt keeps the time line of its starting message. Its model calls see the current time through tools only.

#### Scenario: Context stays fixed within a Run
- **WHEN** a Run makes several model calls across a change of minute
- **THEN** every call of the Run sends the same system prompt and the same starting message

### Requirement: Lazy Tool Loading
The runtime SHALL send only a core set of tool definitions plus the tools loaded in the Thread. The core set includes `find_tools` and `ask_user`. The `find_tools` meta-tool searches all permitted tools by name and description and loads the selected ones for the rest of the Thread. Loaded tools follow the core set in the order they were first loaded. When loading a tool would exceed the tool-definition bound, the earliest-loaded tools are dropped first. A loaded tool that policy no longer permits is dropped before the next request.

#### Scenario: Model discovers a tool
- **WHEN** the model calls `find_tools` with the query "calendar"
- **THEN** it receives matching tool names and descriptions, and from the next model call the selected tools' definitions are included in the request

#### Scenario: Next Run keeps loaded tools
- **WHEN** a Run in a Thread loaded the calendar tools, and the user sends another message
- **THEN** the new Run's first request contains the same tool definitions in the same order as the previous Run's last request

#### Scenario: Forbidden tools are not discoverable
- **WHEN** policy denies a tool for the user
- **THEN** `find_tools` never returns that tool, and a Thread that had loaded it no longer sends its definition

## ADDED Requirements

### Requirement: Stable Prompt Prefix
The runtime SHALL build each model request of a Thread so that it starts with the bytes of the previous request of that Thread. The only exceptions are a compaction, a dropped tool, or a change of Skills, user or folders in between. The system prompt MUST NOT contain anything that changes with each Run. Earlier messages, including tool-call arguments, are resent exactly as stored when the model produced them.

#### Scenario: Next Turn extends the previous request
- **WHEN** the user sends a second message in a Thread without a compaction in between
- **THEN** the second Run's first request equals the first Run's last request plus the newly added messages, byte for byte up to that point

#### Scenario: Compacted Thread
- **WHEN** a Thread was compacted before its last two Turns
- **THEN** both Turns' requests carry the summary wrapped with the same boundary token, the one stored with that compaction

### Requirement: Run Retry
The runtime SHALL let the user retry the latest top-level Run of a Thread when it failed. Retry continues that Run with its starting message and its completed tool results, without adding a user message. A tool call that had started without a result is not executed again; the model receives that it did not complete. Output of a failed model call SHALL NOT become part of any later request. A failed Run that is not retried keeps its starting message in the Thread context.

#### Scenario: Retry after a timeout
- **WHEN** a Run fails with TIMEOUT after two tool calls, and the user chooses Retry
- **THEN** the same Run continues with the next model call, the request contains the user message once and both tool results, and the Thread shows one user message

#### Scenario: Retry after a shutdown during a tool call
- **WHEN** a Run failed with "interrupted by shutdown" while a tool call was executing, and the user chooses Retry
- **THEN** that call is not executed again, the model receives a result saying it did not complete, and the Run continues

#### Scenario: Retry is offered only for the latest Run
- **WHEN** a failed Run is followed by a newer Run in the same Thread
- **THEN** the older Run does not offer Retry
