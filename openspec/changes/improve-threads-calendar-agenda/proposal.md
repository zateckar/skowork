# Proposal

## Why

Using the client day to day showed seven problems in the Thread pane and the pages next to the Inbox:

1. The Daily report links each "Action required" entry to its Thread, but clicking such a link does nothing: links inside model output only open web addresses, so a link to a Thread is refused.
2. The header of a meeting Thread offers "Open the meeting", which opens Settings. The header also repeats the title ("Captured from the meeting <title>") and does not say when the meeting took place or who attended. Other Thread headers repeat their badge in words ("Skowork" next to "Created by Skowork").
3. When the user decides that a triage Thread needs no action, the Thread pane says so, but the list entry keeps its "Needs your action" badge, and the entry stays where it was until the list is loaded again. Changes the client makes to a Thread on its own (triage, meeting capture, Agenda, Daily report) do not reach the list as they happen.
4. The Calendar page shows each day as a stack of buttons. An event's length and its place in the day are not visible, so it does not read as a calendar.
5. The Agenda view puts every item's title, description, due date, quote and six buttons in one undivided column. Items and groups are hard to tell apart.
6. Settings lists its eighteen sections as three rows of identical links above one long page of every section, with no mark of where the user is.
7. Across the other views, times appear as raw UTC timestamps, diagnostics (token lifetimes, message counters, policy version) sit next to what the user needs, and some actions are walls of equal buttons or remedies that cannot work.

## What Changes

- Links in model output that point inside the client (a Thread or another page of the window) open that place in the window, without the browser confirmation. Web links keep their confirmation.
- The Thread header shows each fact once. A meeting Thread shows when the meeting took place and who attended, and links to the calendar event only when one is known; it no longer links to Settings.
- Every change to a Thread's list entry reaches the list as it happens, whichever part of the client made it. Deciding "No action needed" or dismissing a triage suggestion clears "Needs your action".
- The Calendar page's day and work-week modes become a time grid: one column per day, an hour scale, each event placed at its start time with a height proportional to its length, overlapping events side by side, all-day events in a band above the grid, and a line at the current time.
- Settings becomes a two-pane page: a grouped list of sections on the left with the chosen one marked, and only that section on the right. Each section has its own route, and links elsewhere in the window open the right section.
- A first pass of plain information: local, localized times; no internal diagnostics in Status and the status bar; translated category names; one primary remedy in error banners; menus instead of one button per value; a confirmation before forgetting a meeting; plainer labels.
- The Agenda view shows each item as a separate card with a clear hierarchy: title and due date first, sources and preparation state as badges, the quote shortened, the main action as a button, and the less frequent actions in a menu. Each group heading shows its count.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `client/work-inbox`: in-window links in model output, the Thread header, live list entries, the Agenda view layout, Settings navigation, and plain information across views.
- `client/calendar-view`: the time grid of the Calendar page.

## Impact

Roadmap: entry 54, after `add-evaluation-export`. Builds on `add-local-outlook-and-teams` (which introduces `client/calendar-view`, the Agenda draft form and the Daily report) and `improve-triage-results` (Triage Thread Badges, No action needed). Both must be archived first; the delta specs here only add requirements, so they apply on top of either order.

Affected: the window's Markdown links, the Thread header, the meeting progress data (`meetings.progress` gains the meeting's time and attendees), triage decisions (they update the Thread's origin data), the host's notification of list changes, the Calendar page, the Agenda view, the Settings page and its route, and localized strings (en, cs, de). No control-plane, policy or storage schema change.

Out of scope: editing or creating calendar events, drag and drop on the calendar, a month view, a page of its own for a meeting, and changes to what the Agenda detects.
