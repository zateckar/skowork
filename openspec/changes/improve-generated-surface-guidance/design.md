# Design

## Context

`ui::generated_surface_instructions()` is appended to the system prompt of top-level Runs when surfaces are enabled. It describes the JSON contract and the field format of each component type in one paragraph. The `ask_user` tool gains a `form` parameter when surfaces are enabled.

## Decisions

### D1: Say what controls return, not only their shape

The model chose prose questions because nothing told it a control's value would reach it. The instructions now state both return paths: an answer button queues a user message made of the button label and each control's "label: value" (as `message_for` builds it), and an `ask_user` form's button resolves the Question with the same values.

### D2: One short form example

A single compact `ask_user` form example (a slider, a multiselect, one button) shows the shape. The existing answer example (`generated_surface_example`) is not added to the prompt; the field list already covers display components, and the evaluation showed the model composes them correctly.

### D3: Use guidance stays short

One sentence maps purposes to components (data and comparisons: table, bar, line; processes: diagram; choices: controls with a button or an `ask_user` form; short answers: none). The instructions stay a fixed text so the prompt prefix stays byte-stable.

### D4: Forms are for needed input, not offers

Run 2 showed the model using `ask_user` to offer optional extra work ("Would you like me to model scenarios?") before giving the charts it had prepared. The instructions say not to ask in order to offer optional work: answer completely and mention next steps in prose.

## Evaluation

Run 1 (2026-11-17, before this change): 4 of 6 requests produced valid display surfaces; the plain question produced none; both choice requests used prose `ask_user` questions and no controls.

Run 2 (D1-D3): both choice requests asked with a form (the offsite: slider, select, multiselect, button; the laptop: four selects, a slider, a multiselect, button) and the answers used the submitted values. The diagram and comparison requests kept their surfaces and the plain question had none. The ticket-trend request asked an optional follow-up instead of showing the charts (D4 added).
