# Proposal

## Why

A real-gateway evaluation of generated surfaces on 2026-11-17 (six chat requests with surfaces enabled) showed that the model uses the visual half of the catalog well: bar and line charts for a monthly series, a diagram with a stage table for a pipeline, a comparison table, and no surface for a one-line factual answer. All four surfaces it produced were valid.

It never used the interactive half. "Let me set the budget per person, the type of location and the activities" and "Ask me what matters to me" both got an `ask_user` question with a numbered list of sub-questions in prose, not controls. The instructions list the control types but never say what they do: that a button in an answer sends the chosen values back as the user's next message, or that `ask_user` accepts a form whose values return as the answer. The `ask_user` form parameter is described only as "optional version 1 generated UI form with exactly one submit button", and the system instructions do not mention it.

## What Changes

- The generated surface instructions say what the interactive components do: in an answer, a button sends its label and each control's label and value as the user's next message; in an `ask_user` form, the one button returns them as the answer.
- They tell the model to ask with an `ask_user` form, using controls with sensible defaults, when it needs several structured choices before it can continue, instead of listing questions in prose. A short form example shows the shape.
- They say when to use which part of the catalog: charts, tables and diagrams for data, comparisons and processes; controls for choices; no surface for short answers.
- The `ask_user` tool's description of `form` says what the form returns.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `client/agent-runtime`: Generated Surface Instructions.

## Impact

Model-facing text only: `ui::generated_surface_instructions()` and the `ask_user` tool definition. No change to the surface contract, validation, rendering or submission. The instructions grow by about 200 tokens in Runs with surfaces enabled. Verified with the real-gateway evaluation `AppE2E.ModelBuildsLocalAppsAndUsesGeneratedSurfaces`.
