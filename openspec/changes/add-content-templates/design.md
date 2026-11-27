# Design

## D1 Extend generated surfaces, not native cards

Two ways were weighed: the model writes structured JSON per content kind and fixed native cards render it, or the generated surface catalog gains display components and each kind is a template over them. The user chose the second on 2026-11-27. It reuses the surface contract, validation, Citations, state, accessibility and fallback, the components are usable in any answer, and a new kind needs instructions, not native code.

## D2 The six components

All are display components: they do not count as controls and never submit anything. Every component keeps `id`, `type`, `label` and optional `citations`.

| Type | Fields | Limits |
| --- | --- | --- |
| `markdown` | `content` | 8192 bytes; the same markdown subset and link rules as assistant messages (In-Window Links); no images, no raw HTML, no nested `skowork-ui` fences |
| `facts` | `items` [{`label`, `value`, `citations`?}] | 1–20 items; label ≤ 80, value ≤ 512 bytes |
| `callout` | `tone` (info, success, warning, danger), `content` | content as `markdown`, ≤ 2048 bytes |
| `timeline` | `items` [{`time`, `title`, `detail`?, `tag`?, `citations`?}] | 1–50 items; `time` is display text ("09:30", "Mon 14:00") |
| `tasks` | `items` [{`title`, `owner`?, `due`?, `state`?, `link`?, `citations`?}] | 1–50 items; `state` is open, done or overdue; `link` only an in-window route (`#/…`) |
| `draft` | `body`, optional `to`, `cc` (arrays of display strings, ≤ 20), `subject` | body ≤ 16384 bytes, plain text |

`label` is the visible heading of the component, as for tables and charts. `draft` shows recipients and subject above the body, a Copy button that copies the body to the clipboard locally, and nothing that sends. Sending stays where it is today (the Agenda's Send draft and triage's approved `mail.send`).

- **As built (2026-11-27):** fields the table leaves open have these limits: timeline `time` and `tag` ≤ 80 bytes, `title` and `detail` ≤ 4096; task `title` ≤ 4096, `owner` ≤ 200, `due` ≤ 80, `link` ≤ 512 bytes starting with `#/` and without spaces or control characters; draft `subject` ≤ 1024, each `to`/`cc` entry ≤ 4096. Every text field must be non-empty when present. "No nested fences" rejects ` ``skowork-ui` and `~~skowork-ui` in `markdown` and `callout` content. The general surface instructions (setting on) also list the six types, in one line.
- **As built, views:** the header is the template's Segoe Fluent icon in the accent colour, its localized name as a caption, then the title as a heading. `facts` is a list of label (fixed 140 DIP, secondary) and value rows; `callout` a tinted box (info, success = accent surface, warning, error backgrounds) with the tone's icon, a group named "<tone name>: <label>" (the UI Automation roles have no status role); `timeline` and `tasks` are lists whose items carry a spoken name ("09:30 Steering, Teams. Room 4"; "title, state, Owner: …, Due …"); a task with `link` has an "Open" link button that goes through the window's in-window route; `draft` is a bordered box with a caption "To … · Cc … · Subject: …", the body as selectable plain text and a Copy button that also announces the copy.

## D3 Templates

The root may carry `template` (one of `daily_report`, `meeting_summary`, `briefing`, `action_prep`, `meeting_prep`) and `title` (≤ 200 bytes). A template only changes the header: an icon and the template's name in the user's UI language, then the title. It does not constrain the components, so the model can drop an empty section or add a table. The per-kind structure lives in the producer's instructions:

- `daily_report`: `timeline` Today (meetings; busy entries say only "Busy"), `markdown` per topic under Worth knowing with Citations, `tasks` To act on (Agenda items with due dates), `tasks` For your information and Action required with links to their Threads, each only when present.
- `meeting_summary`: `facts` (date, time, participants where known), `markdown` summary, `markdown` decisions as a list, `tasks` follow-ups with owner and due date.
- `briefing` (Agenda preparation): `facts` (asked by, when, due, related references), `callout` for the state when it matters (already answered: success; missing information or overdue: warning), `markdown` brief with Citations, `draft` when the item calls for one, `markdown` or `tasks` for what the user must still supply.
- `action_prep` (triage preparation): `facts` and a `markdown` summary before the Question card; the reply step is unchanged.
- `meeting_prep` (Calendar "Ask Skowork"): `facts` (time, place or link, organizer, attendees), `markdown` what it is about, `tasks` what to prepare or bring, `markdown` background with Citations.

Component labels are written in the content's language, as headings are today.

## D4 Always on for display-only surfaces

The Generated surfaces setting is off by default because surfaces with controls send values back to the model. A surface without controls only presents the answer, like markdown. So a display-only surface renders whatever the setting, including its local exploration state (sorting, collapsed sections). Controls, the `ask_user` form and the general instructions for chat answers stay behind the setting. The runtime marks every complete assistant message with `surface_complete`, and `surfaces.open` and `surfaces.save` accept display-only surfaces when the setting is off.

The template instructions go to the model in the producer's preamble, not the general system prompt, so the setting does not change them.

- **As built (2026-11-27):** the runtime already marked every complete assistant message with `surface_complete` regardless of the setting, so it needed no change; the capture of incomplete streamed text (kept for a stopped Run) stays behind the setting as before. With the setting off, `surfaces.open` and `surfaces.save` look up the bound surface and refuse with `disabled` only when it has a control; `surfaces.submit`, the Question form RPCs and `ask_user` forms stay behind the setting. The feed renders a message natively only when every block in it is supported; with the setting off, a message whose blocks include one with controls stays the readable fallback as a whole.
- **As built, instructions:** `ui::content_template_instructions(kind)` is one shared format paragraph (root, six display types plus table, Citations, `success` rule, no controls) followed by the kind's structure and one example marked as invented data; each is under 3,200 bytes. Producers append it to their own preamble: the Daily report, the meeting summary, Agenda preparation (`briefing`, the draft "as the draft component"), triage preparation (`action_prep`, step 1 points to it; the ask_user and reply steps are unchanged) and the Calendar "Ask Skowork" Run (`meeting_prep`). The triage "Start work" Run is not a preparation and keeps its preamble.

## D5 Fallback, previews and the draft

- A block that fails validation shows the existing fallback (Generated Surface Fallback). Each producer asks for a one- or two-sentence prose summary before the block, so the Thread stays readable.
- Thread previews, toasts and any text excerpt of a message replace a valid surface block with its `summary`, and drop an invalid one.
- `agenda::extract_draft` reads the first `draft` component of a valid surface in the preparation answer, and falls back to today's markdown heading rules.
- **As built (2026-11-27):** the `run_events_inbox` trigger still stores the first 200 characters of a message as the Thread's preview (pure SQL, so older clients keep working). When the inbox service sees an assistant message that holds a `skowork-ui` fence, it replaces that preview with `ui::generated_surface_text` (valid blocks become their summary, other `skowork-ui` fences up to their closing fence or the end are left out), only while the preview is still the trigger's raw text, before the Run's completion toast reads it. It also replaces the message's search-index text with `ui::generated_surface_search_text` (prose plus every text the surface shows, without ids, types, routes or states), so search still finds words inside a surface and its excerpts show no JSON. Previews and search text stored before this change are not rewritten. The Daily report toast has no body and needed no change.

## D6 Risks

- The model can produce invalid JSON more often than markdown. Mitigation: short templates with one example each, unit tests that each example validates, and the fallback.
- A wrong `tone` can overstate a state. The instructions limit `success` to facts the sources show, such as a sent reply with its time.
