# Proposal

## Why

`improve-threads-calendar-agenda` and `improve-plain-information` reworked the Calendar, the Agenda and parts of Settings. A screenshot review of every other screen (the Inbox and its views, a Thread with an Approval, a new Thread, Status, all Settings sections, the Telemetry viewer, About, the Skill and Automation editors, the Automation page, the Knowledge view and Apps) found these problems:

1. Text fields without a visible label. The Automation editor shows six empty boxes; the Skill editor two; Settings > Mail, Knowledge sources, Triage and Daily report each show an empty box next to a button. Their names exist only for screen readers.
2. Times of other seasons are an hour off. The window converts with today's UTC offset, so a Friday 15:00 automation in summer shows "2:00 PM" in winter. Triage times are still shown in UTC; Teams "Last read", the Daily report's last day, Knowledge dates and an App's last use are raw ISO text. Counts have no digit grouping ("18234 messages").
3. The selected tab of the Knowledge view is cut off ("Sourc…", "Document…"): the chosen label is bold, its width was measured regular.
4. The context indicator of the status bar stays on Status, Settings and every other page after a Thread was open, showing the last Thread's usage.
5. Inbox entry previews show Markdown syntax ("## Summary The budget for **Q4**…").
6. The Approval card shows the arguments as raw JSON.
7. Settings detail is still technical in places: grantable folders as "%USERPROFILE%\Documents\**", Policy sources as "· default" and "· group:engineering", Skills' Tools as ids ("files.read"), a rejected Connector token offers "Add token", "0 of 0 tools usable", Status repeats "Connected — Connected to the Skowork control plane." and shows "—" for an unknown server, "1 items expired", a heading "Delete everything…" with an ellipsis, "Verbose diagnostics" followed by "Off", "using the small model" in the Agenda section, and "(read only)" next to "Managed by your organization". The English strings mix "organisation" and "organization".
8. Layout: the remove action of an excluded mail folder or an indexed folder sits right after the text instead of at the row's end; the long Triage option is cut off with an ellipsis; sub-headings inside a section have no space above them; the Daily report time field is empty and the seven working days take seven lines; a captured meeting has an empty line between its title and its actions; the Apps page is an undivided column with five equal buttons per App.

## What Changes

- Every text field shows its label as placeholder text while empty; fields of the Automation editor, the Skill editor and the Daily report time also show the label above the field.
- Dates and times use the UTC offset in effect at that moment and one European format in every language: 24-hour time ("15:00") and day.month.year ("27.11.2026"). All remaining raw or UTC times use it. Counts use the language's digit grouping.
- Tabs are measured with the font they are drawn with.
- The context indicator shows only while a Thread is shown.
- Previews show text without Markdown syntax.
- The Approval card shows arguments as name and value lines; Edit arguments keeps the JSON editor.
- Plain wording for the items in 7; grantable folders read like "Documents and its subfolders"; Policy shows a group source as "Group engineering" and omits the default; Skills list Tools by name; a rejected token offers Replace token; Status shows each fact once and hides unknown values; US spelling throughout the English strings.
- Settings rows: the action of a removable item at the row's end, wrapping option labels, space above sub-headings, the Daily report time filled in and the working days on one wrapping row, no empty line in captured meetings, Apps as cards with Open, a Running badge and a More menu.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `client/work-inbox`: labelled fields, local dates and grouped numbers, tab measurement, context indicator scope, plain previews, readable Approval arguments, plain Settings details and Settings row layout, Apps cards.

## Impact

Roadmap: entry 56, after `improve-plain-information`, whose Plain Settings Details requirement this extends; both earlier changes add requirements only, so this applies on top of either order of archiving. Affected: the window only (`client/src/ui/textfield.cpp`, `client/src/ui/elements` tab drawing in the Knowledge view, `client/src/shell/*`), and localized strings (en, cs, de). No host, storage, policy or control-plane change.

Out of scope: hiding the empty Activity rail, a schedule picker in place of the cron field, and moving Settings > About out of Settings.
