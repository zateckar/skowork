# Design

## Context

The feed model (client/src/shell/feed.*) turns Run events into items: user, assistant, reasoning, tool, activity, action, error, stopped, working. The view builds a card per item. The decision of what to show belongs to the view; the feed keeps every item so details can be revealed without reloading.

## Decisions

### D1. Filter in the view, keep the items
- **Choice:** the feed keeps all items. The view skips reasoning and tool items of a Run unless the setting is on or the Run is in the set of Runs whose details the user opened (kept per window, not persisted). Activity items that only describe tool steps are hidden the same way; errors, Action items, stopped notices and produced files and images are always shown.
- **Alternatives considered:** dropping the events in the feed model (details could not be revealed later); a per-Thread setting (more state for little gain).

### D2. Status line
- **Choice:** while a Run is running, the existing "working" item shows the step label of the last tool call, mapped from the tool group (files.* → "Reading files" or "Writing files" by risk class, web.* → "Searching the web", browser.* → "Using the browser", mail.* → "Working with mail", knowledge.* → "Searching knowledge", mcp:<server>/* → "Using <server>", other → "Working"), or "Thinking" while only reasoning streams. No arguments or results appear.
- **Alternatives considered:** a model-written summary of each step (extra model calls and latency).
- **Settled during implementation:** further groups have their own labels: computer.* → "Using the computer", shell.* → "Running commands", calendar.* with mail.* → "Working with mail", image.* → "Creating an image". While the answer streams the line is the plain "Working". The line stands in for the live reasoning block when details are hidden.

### D4. Details button and produced outputs (settled during implementation)
- **Choice:** Show details appears after the last item of every Run that has hidden work and is not queued or running, so a Run waiting for an approval or answer offers it too (the user may want to see what led to the request). A tool call that stored images or returned citations keeps those under the answer flow without its card. The usage line stays.

### D3. Setting
- **Choice:** `ui.show_work_details` in the user settings store, default false, shown in Settings > General as a check box.
- **Alternatives considered:** a policy key (administrators have no reason to force it).

## Risks / Trade-offs

- [Users miss why a Run failed] → Errors are always shown, and Show details is one click away on every finished Run.
- [Golden screenshots change] → The feed reference images are regenerated and reviewed.
