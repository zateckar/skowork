# Spec Delta

## MODIFIED Requirements

### Requirement: Context Compaction
Before each model request, the runtime SHALL compact older context when its estimated input reaches the proactive trigger defined by Working Context Budget. It first lets BeforeCompaction handlers flush durable facts, then preserves system instructions, the latest user message, unresolved tool calls and their results while summarizing older context. This applies to attended, Unattended and child Runs without changing recorded approvals or tool execution state.

#### Scenario: Long run
- **WHEN** a Run's next request reaches the proactive trigger for its model
- **THEN** the runtime compacts older context before sending it and records the result in the Thread's Activity log

#### Scenario: Long single Run
- **WHEN** one Run accumulates enough completed tool exchanges to reach the trigger without a new user Turn
- **THEN** those completed exchanges can be compacted and the Run continues without replaying them

#### Scenario: Child Run
- **WHEN** a child Run reaches its model's trigger
- **THEN** its own context is compacted without including private context of another Run or widening its permitted tools

## ADDED Requirements

### Requirement: Core Runtime Tool Identities
The runtime SHALL identify its reserved built-in Tools as `find_tools`, `ask_user`, `read_attachment`, `delegate` and `use_skill`. Their behavior remains owned by the runtime requirements and, for Skill execution, client/skills; their presence in a catalog does not imply that a particular Run can use them.

#### Scenario: A reserved Tool is not offered
- **WHEN** a Run cannot use `use_skill`
- **THEN** the administrative catalog can still identify that Tool without promising it is available to the Run

### Requirement: Working Context Budget
The runtime SHALL use an input budget B equal to the minimum of the model-tier cap, 80% of its context window W, and W minus the reserved output and safety margin. The tier cap is 500,000 tokens when W is at least 1,000,000, otherwise 200,000. The safety margin is the greater of 4,096 tokens and 2% of W. Compaction triggers at 90% of B and targets at most 60% of B.

#### Scenario: One-million-token model
- **WHEN** W is 1,000,000 with an 8,192-token output reserve
- **THEN** B is 500,000, compaction starts at 450,000 estimated input tokens and targets at most 300,000

#### Scenario: 262k-token model
- **WHEN** W is 262,144 with an 8,192-token output reserve
- **THEN** B is 200,000, compaction starts at 180,000 estimated input tokens and targets at most 120,000

#### Scenario: Smaller or output-heavy model
- **WHEN** the model's window or its requested output leaves less room than the tier cap
- **THEN** the runtime uses the smaller budget and refuses a request with no positive input allowance

#### Scenario: Output limit is not reported
- **WHEN** the selected chat role does not report a positive output limit
- **THEN** the runtime requests at most 4,096 output tokens and reserves that amount in the input budget

### Requirement: Complete Request Accounting
Budget checks SHALL include instructions, summaries, conversation messages, tool names and schemas, tool arguments and results, image inputs and message framing. Output reservation SHALL cover the request's permitted completion, including reasoning where the model counts it in the context window. Estimates SHALL be identified as estimates and reconciled with provider usage when available.

#### Scenario: Large tool catalog
- **WHEN** messages alone fit but messages plus tool definitions reach the trigger
- **THEN** compaction runs before the request, and the reported context usage includes the definitions

#### Scenario: Context indicator uses the working budget
- **WHEN** the Thread view shows context usage and the runtime reports its input budget
- **THEN** the indicator uses that budget as its denominator and identifies the value as an estimate, including any omissions in its accessible explanation

#### Scenario: Tool selection can change between requests
- **WHEN** the displayed context estimate uses the most recently attempted request's tool definitions
- **THEN** its explanation identifies them as the last selection and does not imply that the next request has the same tools

#### Scenario: Tool-call repair exceeds the budget
- **WHEN** a corrective model request would exceed the current input budget after adding the invalid call and its validation errors
- **THEN** the client does not send that corrective request and retains the original validation errors for the Run

### Requirement: Model-Aware Budget Refresh
The runtime SHALL recompute the budget when the selected Model role, resolved model or reported limits change. Missing limits SHALL use the conservative 32,768-token fallback with the same reserves, without inferring capacity from a model name.

#### Scenario: Switch to a smaller model
- **WHEN** a Thread moves from a 1M model to a 262,144-token model
- **THEN** the next request is checked against the 200,000-token or lower budget before it is sent

### Requirement: Compaction Coverage
A compaction SHALL account for every message removed from active context and the complete prior summary through bounded summarization passes. It MUST NOT silently omit earlier messages or evidence merely because the summarizing model has a smaller window. Goals, explicit constraints, decisions, pending work and evidence references SHALL remain represented.

#### Scenario: Small summarizer window
- **WHEN** the removed conversation is larger than the summarizing model's usable input
- **THEN** bounded passes cover the whole removed range, including its beginning and prior summary, before a replacement checkpoint is committed

### Requirement: Compaction Trust Preservation
Compacted content SHALL preserve the distinction between user instructions and untrusted Source or Tool content. A generated summary MUST NOT become a new authority for permissions, approval, identity or completion of an external action.

#### Scenario: Injection in an old document
- **WHEN** a compacted document says the user approved sending private data
- **THEN** the summary retains it only as a claim from that document, and policy and approval state remain unchanged

### Requirement: Atomic Compaction Checkpoint
Compaction SHALL replace active context only after a valid nonempty summary and a consistent conversation boundary are persisted atomically. Cancellation, failure, concurrent user edits or restart MUST preserve a recoverable prior context and the authoritative tool-call and approval state.

#### Scenario: New message during compaction
- **WHEN** the user adds a message while a summary is being generated
- **THEN** the checkpoint cannot discard that message or overwrite a newer context revision

#### Scenario: Crash before commit
- **WHEN** the process stops before the new checkpoint is committed
- **THEN** restart uses the prior complete context and does not replay a completed tool action

### Requirement: Compaction Failure And Oversize Input
After compaction the runtime SHALL recheck the full request. It MUST NOT knowingly send a request at or above B. If safe compaction cannot make it fit, the Run SHALL stop before the model call with an actionable context error and intact history. A failed compaction below B permits continuation only with a visible failure and bounded retry behavior.

#### Scenario: Latest message alone exceeds budget
- **WHEN** the latest user message and mandatory context cannot fit below B
- **THEN** the client asks the user to shorten or attach the input, without silently truncating it or repeatedly retrying the same request

#### Scenario: Summarizer unavailable
- **WHEN** compaction fails because the small Model role is unavailable and the full input is at or above B
- **THEN** the Run reports that compaction could not complete and can be retried after model access returns

### Requirement: Inspectable Compaction
The client SHALL show context usage, the effective budget and compaction progress, and record before/after estimates and the reason in the Activity log. Compaction SHALL retain the original readable Thread history under its existing retention and forget rules.

#### Scenario: Read a compacted Thread
- **WHEN** a user opens a Thread after automatic compaction
- **THEN** earlier messages remain readable and the Activity log identifies the summary checkpoint and budget reduction
