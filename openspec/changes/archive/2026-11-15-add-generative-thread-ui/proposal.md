# Proposal

## Why

Thread answers are mostly prose, even when a comparison, diagram, chart or set of choices would let the user understand and respond more easily. The agent needs a way to compose useful native interfaces for the task while keeping the user's intent, approvals and source evidence clear.

## What Changes

- Let the agent compose bounded native visual and interactive surfaces within assistant messages in a Thread, alongside ordinary text.
- Support accessible tables, simple charts and relationship diagrams, plus forms with selections, switches, text fields and explicit submission. Support local sorting and filtering without another model call.
- Return a submitted response to the correct Thread or pending Question, preserving explicit user control and existing tool policy and approvals. Persist surfaces and interaction state safely across streaming and restart.
- Teach the agent the available component catalog, provide plain-text fallback and source references, and constrain untrusted descriptions and resource use.
- Validate model output quality and native performance with a spike before committing to the wire format and renderer implementation.

## Capabilities

### New Capabilities

None. Presentation belongs to client/work-inbox and Run interactions belong to client/agent-runtime.

### Modified Capabilities

- `client/work-inbox`: present generated visual surfaces and collect explicit user input in the Thread feed, with accessible fallback and local interaction.
- `client/agent-runtime`: advertise the supported surface contract to the model and route submitted values to the correct Run or a new user message.

## Impact

- Roadmap: productivity change after add-native-ui, add-work-inbox, add-agent-runtime, fix-feed-live-updates and stabilize-agent-tool-catalog; it uses their behavior and depends on resolving conflicts with their in-flight specifications. Existing policy decisions remain owned by client/policy-enforcement.
- Affects the native Thread feed, Markdown parsing, native controls and accessibility, Run events, Question handling, persistence and model prompting. It does not require a client web engine or a control-plane service.
- Out of scope: arbitrary generated HTML, scripts or remote resources in the client, 3D scenes, opening Local apps, MCP Apps hosting, changes to tool authorization or approval policy, and adding a new model provider. Local apps remain an independent, existing capability.
