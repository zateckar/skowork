# Proposal

## Why

A review of every view against "show exactly what the user needs for the task" found issues that `improve-threads-calendar-agenda` (design D7) left out, because each reshapes a view or a host contract:

- Automation times are formatted by the scheduler as "2026-11-27 09:00" in the Trigger's time zone, and schedules read as cron expressions ("Cron 0 8 * * 1-5 (Europe/Prague)").
- The Approval card is titled with the raw Tool id and lists sources by internal ids (message ids, full file paths).
- When an interrupted Run cannot continue, the banner shows an untranslated developer sentence from the runtime.
- Settings > Skills shows file paths, versions and the raw risk value of every Skill; Settings > Policy shows the version number and raw maps such as "read = allow; external = approve".
- Each Knowledge source has four equal buttons, its on/off is a button where every other setting is a switch, and skipped items show raw ids and reason codes.
- Each Automation row has a switch and three equal buttons.
- The Knowledge view marks its chosen section and Source tab only by a button style, without a selected state for UI Automation.
- A team Chat's header is a wall of four buttons above a line listing every member with their presence.
- Gaps between the items of similar lists differ from view to view.

## What Changes

- Automation times are shown in local time in the window's format, from the times the host already sends; common schedules are described in words ("Workdays at 08:00"), others read "Custom schedule" with the expression in the tooltip.
- The Approval card names the Tool in words and lists sources by what they are (a file name, a web host, "Email") with the full reference in the tooltip.
- The runtime reports why a Run cannot continue as a code; the banner shows a translated reason.
- Skills show their description, state, translated risk and Tools; source, version and other copies are behind a Details toggle. Policy shows translated decisions and risk names and "Valid until …" instead of the version.
- Knowledge sources get a switch plus a More menu (Pause, Reindex, Forget); skipped items show the file name and a translated reason.
- Automation rows keep their switch; the name opens the history; Run now and Edit move to a More menu.
- The Knowledge view's sections and Source tabs become tabs with a selected state.
- A team Chat header keeps "Add people" as a button and moves Rename, Save as note and Leave to a More menu; members read "N members", with names and presence in the tooltip.
- Settings lists use one spacing (16 DIP between items of several lines, 8 between one-line items).

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `client/work-inbox`: plain information in the Approval card, Settings sections, Knowledge view tabs and Chat header.
- `client/agent-runtime`: the reason an interrupted Run cannot continue is a code.

## Impact

Roadmap: entry 55, after `improve-threads-calendar-agenda`, whose Plain Information requirement this extends; that change is archived first. Affected: the runtime's recovery status (`runs.recovery` gains `blocker_code`; `blocker` stays for compatibility), the Approval card, the Settings sections for Skills, Policy, Knowledge sources and Automations, the Automation page, the Knowledge view, the Chat pane, and localized strings (en, cs, de). No storage, policy or control-plane change.

Out of scope, recorded in design.md D7: translating meeting-capture error texts (the meeting service stores them as sentences) and moving the captured-meetings list out of Settings.
