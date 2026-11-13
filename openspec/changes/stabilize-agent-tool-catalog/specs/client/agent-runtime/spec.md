# Spec Delta

## MODIFIED Requirements

### Requirement: Lazy Tool Loading
The runtime SHALL offer a deterministic baseline Tool catalog on every model request, plus Tools loaded lazily in the Thread. The baseline contains the reserved runtime Tools, files.read, files.write and files.edit. While client/local-apps offers them, apps.create and apps.open are also baseline definitions. files.list, files.glob, files.grep, apps.list, apps.logs, apps.stop, apps.delete and all non-reserved Tools outside the baseline are discovered with `find_tools` and follow the baseline in first-load order.

These baseline file and Local app definitions SHALL be available from the Run's first model request, without a discovery call. Existing availability and permission restrictions MUST continue to apply. When no longer offered or permitted, a Tool is not sent. A Tool now present in the baseline MUST NOT be emitted twice because an earlier Thread loaded it.

`find_tools` SHALL return only available, permitted non-reserved Tools as discoverable, loadable matches. A denied or unavailable Tool MUST NOT be loaded or made callable. An explanation required by client/policy-enforcement MAY accompany the result, but is not a loadable match. A loaded Tool that policy no longer permits MUST be dropped from the next request.

Promoting a Tool to the baseline MUST NOT narrow the Tool set a Child Run inherits when its delegate call omits an explicit `tools` list. That default set includes the parent's available, permitted non-reserved baseline Tools and its available, permitted Thread-loaded Tools. An explicit Child Run Tool set remains restrictive, and a Child Run MUST NOT obtain a Tool its parent could not provide.

The combined baseline and lazily loaded definitions SHALL remain within the 4,000-estimated-token Tool-schema budget. Budget comparisons SHALL use the complete provider-effective Tool schemas, including policy-dependent additions such as a required rationale argument. When loading another Tool would exceed the budget, the runtime drops the earliest lazily loaded Tool first.

#### Scenario: Build an app without discovery detours
- **WHEN** a user asks for a Local app while Local app Tools are offered
- **THEN** the first model request already contains apps.create, apps.open, files.read, files.write and files.edit, so the model can create, write and open the app on consecutive model Turns without find_tools

#### Scenario: Model discovers another tool
- **WHEN** the model calls `find_tools` with the query "calendar"
- **THEN** it receives matching tool names and descriptions only for available, permitted non-reserved Tools, and from the next model call the selected definitions follow the unchanged baseline in first-load order

#### Scenario: Forbidden tools are not discoverable
- **WHEN** policy denies a non-baseline Tool whose name matches a `find_tools` query
- **THEN** the result does not offer that Tool as a loadable match or emit a tools-loaded event for it, and any explanation follows client/policy-enforcement's Withheld Tool Explanation
- **AND** if a Thread had previously loaded that Tool, its definition is dropped from the next request

#### Scenario: Default Child Run keeps baseline and loaded tools
- **WHEN** a parent delegates without an explicit `tools` list and has never recorded a baseline Tool in its legacy lazy-load history
- **THEN** the Child Run's fixed Tool set includes the parent's available, permitted non-reserved baseline and Thread-loaded Tools
- **AND** an explicit `tools` list still restricts the Child Run to the requested subset that the parent can provide

#### Scenario: Old Thread contains a baseline tool in its load history
- **WHEN** a Thread resumed after this change previously loaded files.write through find_tools
- **THEN** the request contains the files.write definition exactly once, without repeating or reordering the baseline

#### Scenario: Forbidden or unavailable tools are not offered
- **WHEN** policy denies a Tool, Local app execution is unavailable, or a Child Run's fixed Tool set excludes it
- **THEN** its definition is not sent, and availability of that Tool has not been widened by the baseline rule

#### Scenario: Next Run keeps loaded tools
- **WHEN** a Run in a Thread loaded calendar tools, and the user sends another message
- **THEN** the new Run's first request contains the unchanged baseline followed by those Tools in their previous load order

### Requirement: Stable Prompt Prefix
The runtime SHALL build each model request of a Thread so its provider-effective system message, Tool catalog and earlier canonical messages stay byte-stable while later canonical messages are appended. The only allowed changes to an earlier prefix are a compaction, a Tool-catalog revision, or a change of Skills, user or folders. A Tool-catalog revision includes adding, dropping, withdrawing or changing a Tool definition and is an explicit prefix-cache boundary. After such a revision, the new catalog and message history remain stable until the next allowed change.

The existing one-shot image message required by Image Tool Results is an exception to the whole-request append-only comparison. It is appended after the unchanged canonical messages only for the request immediately following its Tool result and dropped from the next request. Its removal is not a Tool-catalog revision and MUST NOT change the provider-effective system message, Tool catalog or any earlier canonical message. Image bytes remain transient as Image Tool Results requires.

Canonical messages are the stored conversation messages, including raw Tool-call arguments, excluding the transient provider-only image message. Earlier canonical messages, including valid unrepaired Tool-call arguments, are resent exactly as stored when the model produced them. Stability comparisons MUST include complete Tool definitions, not just Tool ids.

#### Scenario: App construction extends a stable catalog request
- **WHEN** consecutive model Turns create and write a Local app without compaction, availability changes or other catalog revisions
- **THEN** every request carries the same complete baseline definitions, including apps.open after creating and writing files, and all messages from the preceding request are resent byte-for-byte before the newly appended messages

#### Scenario: Next Turn extends the previous request
- **WHEN** a user sends a second message in a Thread with the same system context and Tool catalog
- **THEN** the second Run's first request preserves the first Run's last complete request content and appends the intervening answer and new user message

#### Scenario: Discovery creates an explicit catalog revision
- **WHEN** find_tools adds a previously unloaded Tool
- **THEN** the next request may differ at the Tool-catalog boundary, but later requests with that catalog keep every earlier Tool definition and message byte-stable

#### Scenario: One-shot image projection is removed
- **WHEN** a Tool result carries an image and the Run continues without any other allowed prefix change
- **THEN** the immediately following request alone appends the image message after byte-stable canonical messages
- **AND** the next request drops only that transient image message while preserving the provider-effective system message, complete Tool catalog and every earlier canonical message

#### Scenario: Compacted Thread
- **WHEN** a Thread was compacted before its last two Turns and has no Tool-catalog revision between them
- **THEN** both Turns' requests carry the summary wrapped with the boundary token stored with that compaction
