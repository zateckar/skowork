# Proposal

## Why

On 2026-10-31 the product owner asked that the chat show only what is relevant to the end user. Today every Run in the Thread feed shows the model's reasoning blocks and a card per tool call with its arguments and result (client/work-inbox Streaming Thread Feed). For most users this is noise: they want the answer, what needs their decision, and what went wrong.

## What Changes

- By default the Thread feed shows the user's messages, the agent's answers, Action items, errors, and the files and images a Run produced. Reasoning and tool-call cards are hidden.
- While a Run is active, one status line says in plain words what the agent is doing (for example "Reading files"), without arguments or results.
- Each finished Run offers "Show details", which reveals its reasoning and tool-call cards for that Run.
- A new setting in Settings > General, "Show work details" (off by default), shows reasoning and tool-call cards for every Run, as today.

Out of scope:
- Changes to what is recorded: the Activity log, telemetry and the approval log keep everything.
- The Activity rail of subagent Runs.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/work-inbox`: Streaming Thread Feed (modified), Work Details Visibility (added).

## Impact

- **Roadmap:** 17, after add-knowledge-sharing. Depends on add-work-inbox and add-native-ui.
- **Code:** client/src/shell (feed model and feed view, settings), strings in en, de, cs.
- **Data:** one user setting stored with the other General settings.
