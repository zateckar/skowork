# Design

## Context

- Markdown links are `LinkSpan` nodes (`client/src/ui/richtext.cpp`). `LinkSpan::invoke` calls `Ui::open_link`, and `Window::open_link` accepts only `http`, `https` and `mailto`. The Daily report writes `[Thread](#/threads/<id>)` (`client/src/report/report_core.cpp`), so the click beeps.
- The meeting service gives a meeting Thread the origin ref `{meeting_id, title, link: "#/settings"}` (`client/src/meeting/meeting_service.cpp`): Settings once held the meeting list. The header (`client/src/shell/views_thread.cpp`) prints "Captured from the meeting <title>" and the link.
- Triage, meeting capture, the Agenda, the Daily report and Remote tasks write the inbox store directly (`client/src/app/app.cpp`). Only `InboxService` emits `inbox.changed`, so their writes reach the window only when something else reloads the list. Triage's "No action needed" archives the Thread this way.
- "Needs your action" of a triage Thread is `has_action || (origin_ref.action == "required" && no Run yet)` (`client/src/shell/model.h`). "No action needed" leaves `action: required` in the origin ref.
- The Calendar page (`client/src/shell/views_calendar.cpp`) and the Agenda view (`client/src/shell/views_agenda.cpp`) are built from generic page helpers: labels, buttons and boxes in columns.

## Decisions

### D1. In-window links go through a route hook on `Ui`

`Ui` gains `open_route`, set by the Shell to `navigate`. `LinkSpan::invoke` sends targets that start with `#/` or `skowork://thread/` to `open_route` (the latter as `#/threads/<id>`), and everything else to `open_link` as before. `navigate` already sends an unknown route to the Inbox, and a missing Thread shows the Thread pane's not-found state.

Alternatives: let `Window::open_link` accept `#/` (rejected: the window layer does not know routes, and the confirmation dialog would still show); rewrite report links to `skowork://` (rejected: the web-UI route strings are already the window's link format).

### D2. One place for Thread header facts; meeting facts from `meetings.progress`

The origin line shows the badge plus text only when the text adds something. System Threads show only the badge (or the triage category badge). Meeting Threads show the meeting facts instead of "Captured from the meeting <title>".

`meetings.progress` already loads once per meeting Thread. It gains `started_at`, `ended_at` (ms; detection time when capture never started), `attendees` (names) and `event_id`. The header renders a "when · length · attendees" line from it. Attendees: the first three names, then "+N". The link "Show in calendar" appears only when the meeting was matched to a calendar event (`event_id`): it opens the Calendar page in week mode on the meeting's week with that event's details shown. New meeting Threads get no `link` in their origin ref; old Threads keep `#/settings` in theirs, and the header ignores the origin ref's link for meetings.

Alternatives: a meeting page of its own (out of scope); link to the event's web or Outlook link (rejected: the local Outlook backend has no web link, and leaving the window for something Skowork can show itself is a detour).

### D3. The host emits `inbox.changed` for every direct store write

`InboxService` gets a public `thread_changed(id)` (the existing private `changed`). The `app.cpp` adapters that write the inbox store for triage, meeting capture, Agenda preparation, the Daily report, calendar Ask and Remote tasks call it after the write. `inbox.changed` is posted to the main thread and carries the whole entry, so duplicates are harmless.

Triage's `no_action` and `dismiss` rewrite the Thread's origin ref with `action: "none"` (keeping kind, scope, item, category), so the derived "Needs your action" clears even when the Thread never had a Run.

Alternatives: a change callback inside `InboxStore` (rejected: the store is also used by `InboxService` itself, which already emits, and by tests without a window; it would double every event and make the store depend on threads); reload the whole list on `triage.changed` (rejected: covers triage only).

### D4. The time grid is one custom node

A `CalendarGrid` node draws the hour scale, the day columns, the hour lines and the now line, and places one focusable block per event with `arrange_children`. The node also holds the all-day band: all-day events and events that cover whole days are blocks in rows under the day names. The page keeps the toolbar and the details card.

- Scale: 48 DIP per hour. Range: 08:00–18:00, extended to whole hours around the earliest start and latest end of the shown events, capped to 00:00–24:00. The grid scrolls with the page. The sync time is shown in local time instead of the raw timestamp.
- Overlaps: per day, events sorted by start are packed into clusters of transitively overlapping events; each event takes the first free lane, and lanes share the column width. This is the common calendar layout and costs O(n²) per day for the few dozen events of a week.
- Blocks: minimum height 20 DIP; text is the start time and subject, then location if it fits; private and withheld events read "Busy" and cannot be opened. The chosen event is drawn in the accent colour. Blocks are buttons for UI Automation, named "<day> <start>–<end> <subject>".
- No automatic scrolling: the grid starts at 08:00 or at the hour of the earliest event, so the morning is in view when the page opens. The chosen event's details sit above the grid, so they are seen without scrolling.

Alternatives: absolute positioning in the generic layout (rejected: the layout engine has no absolute children); drawing the events without child nodes (rejected: no focus or UI Automation per event).

### D5. Agenda items as cards with an overflow menu

Each item is a `Box` with the surface colour, a border, 8 DIP radius and 12 DIP padding, in a list with 8 DIP gaps. Order inside: a row with the title (strong, wrapping) and the due chip (a badge: warning colours when overdue, "inferred" added when it is), the action (secondary text), a row of badges (source kinds; "Prepared"), the quote as one cut-off caption line, and an action row: the primary Prepare / Open preparation, Open source, Mark done, Reply draft where it applies, and a "More" icon button that opens a `Menu` with Dismiss, Edit date and Rename. Group headings read "Overdue · 3". The "N new items are waiting to be checked" note becomes a caption at the top.

Alternatives: a two-column table (rejected: titles and quotes vary too much in length for the narrow pane); hiding all actions behind hover (rejected: not reachable by keyboard and screen reader in the same way).

### D6. Settings as list and detail

The Settings page becomes a row: a 220 DIP column with the grouped section list (subtle buttons, the chosen one drawn as selected and exposing SelectionItem), and the content column with the chosen section's card only. The route gains a section (`#/settings/<section>`); `#/settings` uses the section chosen last in this window session, General at first. Links that used to open `#/settings` open the section they mean (mail, agenda, meetings, teams, daily report, triage). Only the chosen section's data is loaded.

Alternatives: keep one long page and make the list a sticky table of contents (rejected: the page holds eighteen sections and the toolkit has no sticky children; the user still scrolls past unrelated settings); tabs across the top (rejected: eighteen labels do not fit one line, which is today's problem).

### D7. A first pass of plain information

A review of the other views listed what the user sees that they do not need, or cannot read. This change fixes the contained items: the three helpers that printed "YYYY-MM-DD HH:MM UTC" and the meetings list's ISO times now use the window's localized moment format; the quota reset time likewise; Status drops the token lifetime and the last acknowledged message number (the Installation ID stays, as support asks for it); storage categories get translated names; the status bar no longer says "Policy 7" while all is well; the context-length banner makes Compact primary and drops Retry; Pause is one button with a menu; forgetting a meeting asks first through the dialog Knowledge already uses; "Steer now", "Control plane", "Shadowed" and the realm wording become plain language.

Left for a follow-up change, because each reshapes a view or a host contract: automation times formatted by the scheduler, raw tool ids and source ids on the Approval card, recovery blocker texts from the runtime, Skills and Policy detail rows, the Knowledge sources and automation row actions, the meeting list's place in Settings, the cron wording, the Knowledge view's tab selection, the team chat header, and spacing tokens across lists.

Alternatives: fix every review item here (rejected: several need host changes and their own specs); leave them all for later (rejected: the timestamp and banner items are small and visible every day).

## Risks / Trade-offs

- [A route link in model output could send the user somewhere unexpected] -> Only in-window routes open without confirmation; they cannot run anything or leave the window, and every action on the target still needs its own click.
- [More `inbox.changed` events while triage processes a batch] -> Each is one small JSON posted to the main thread; the list updates one entry in place. A batch of 50 items costs well under a frame on the baseline laptop.
- [Grid unreadable for many short overlapping events] -> Lanes keep each block at least 20 DIP high, the name and tooltip carry the full text, and the list mode remains.
- [Tests and links that open `#/settings` and expect every section] -> Links are moved to section routes; tests open the section they check.
- [Old meeting Threads keep `#/settings` in their origin ref] -> The header no longer reads the link for meetings, so no migration is needed.
