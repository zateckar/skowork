# Proposal

## Why

Skowork writes the same kinds of content into Threads again and again: the Daily report, meeting summaries, Agenda briefings, triage action preparations and meeting preparations from the Calendar. Today each one is a wall of model-written markdown. The structure is always the same: who asked and by when, the status, decisions and follow-ups with owners and dates, a timeline of the day, a draft reply. Markdown loses it. Important states, such as "you already replied", read like any other bullet.

Generated surfaces (add-generative-thread-ui) can show structured content natively, but the catalog has no components for this content: no key facts, no status callout, no timeline, no task list with owners and due dates, no message draft. A paragraph cannot hold links or Citations. Surfaces are also off by default, behind a setting that exists because of their interactive controls.

## What Changes

- Six display components join the generated surface catalog: `markdown` (rich text with links and Citations), `facts` (label and value pairs), `callout` (a status line with a tone), `timeline` (timed entries), `tasks` (work with owner, due date, state and an in-window link) and `draft` (a message draft with recipients, subject, body and Copy).
- A surface may name a Content template (`daily_report`, `meeting_summary`, `briefing`, `action_prep`, `meeting_prep`) and a title. The Thread shows a header with the template's icon and localized name, then the title.
- The Daily report, meeting summaries, Agenda preparation, triage action preparation and Calendar meeting preparation ask the model for their Content template, with a short prose summary before the block. The template instructions are part of each producer's instructions.
- Display-only surfaces (no controls) render whether or not the Generated surfaces setting is on. Controls, forms and the general surface instructions for chat answers stay behind the setting.
- Thread previews, notifications and the Agenda draft extraction read a surface's text, not its JSON.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `client/work-inbox`: Generated Thread Surfaces and Generated Surface Setting (modified); Content Templates and Surface Text In Previews (added).
- `client/agent-runtime`: Content Template Instructions (added).
- `client/agenda`: Prepared Brief Presentation (added).
- `client/daily-report`: Daily Report Presentation (added).
- `client/meeting-capture`: Meeting Summary Presentation (added).
- `client/event-triage`: Action Preparation Presentation (added).
- `client/calendar-view`: Meeting Preparation Presentation (added).

## Impact

- `ui/generated_surface_protocol` (validator, catalog, template instructions), new native views in `ui/` for the six components and the template header, `shell/views_feed` (display-only surfaces without the setting), `runtime` (surface completion and `surfaces.open` for display-only surfaces), `agenda/core` (`extract_draft` reads a `draft` component), Thread previews and toasts, the five producers' instructions, strings (en, cs, de), the fuzz target.
- Model output grows by the JSON structure; each producer's instructions grow by about 300 tokens.
- Older clients show a template surface as its fenced JSON (Generated Surface Fallback).
