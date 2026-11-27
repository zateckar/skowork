# Design

## Context

The review behind `improve-threads-calendar-agenda` D7 listed these items. Relevant code: `client/src/scheduler/scheduler.cpp` (`to_json` sends `next_fire_at`/`fire_at` and preformatted `*_text`), `client/src/shell/views_automations.cpp`, `client/src/shell/views_feed.cpp` (ApprovalCard), `client/src/runtime/run_store.cpp` (`continuation_blocker` returns English sentences) and `runtime.cpp` (`runs.recovery`), `client/src/shell/views_pages.cpp` (Skills, Policy), `client/src/shell/views_knowledge.cpp` (Knowledge sources), `client/src/shell/views_knowledge_view.cpp` (section tabs, Source chips), `client/src/shell/views_chats.cpp` (Chat header).

## Decisions

### D1. Times formatted by the window from epoch values
The scheduler already sends `next_fire_at` and `fire_at` (ms). The views format them with `Shell::moment` and fall back to the `*_text` value. The scheduler keeps the text, so other readers (the Triggers tool output) are unaffected.
Alternatives: drop the text from the scheduler (rejected: the Triggers tool output reads it).

### D2. Schedules in words, in the shell core
`describe_schedule(cron)` in `client/src/shell/format.cpp` recognises five-field crons with a fixed minute and hour: `* * *` daily, `* * 1-5` workdays, `* * <one weekday>` weekly, and `m * * * *` with a fixed minute hourly; anything else is custom. It is portable and unit-tested. The zone is not shown (times are local); the expression and zone go to `help`.
Alternatives: a full cron-to-text translator (rejected: too wide for little benefit).

### D3. Tool names and source references in the window
Strings for the built-in Tools (`tools.names.<id>`, for example "Send an email" for mail.send). `mcp:<server>/<tool>` and `api:<connector>/<op>` read "<tool> (<server>)"; unknown ids show as they are. A source reference comes from its kind: files take the last path segment, web the URL's host, everything else the kind's label only. No runtime change; the Provenance stays as it is.
Alternatives: titles in the Provenance (rejected for now: every Tool would need to provide one).

### D4. Blocker codes
`RunStore::continuation_blocker_code` returns the code; `continuation_blocker` maps the code to the old English text for errors and logs. `runs.recovery` adds `blocker_code`. The banner uses `thread.recovery.blockers.<code>`, with a generic text for an unknown code.
Alternatives: translate the English sentences in the window (rejected: brittle against wording changes).

### D5. More menus and Details toggles
A More menu is `IconButton(icon::more)` opening a `ui::Menu`, as in the Agenda cards. Menu items keep the automation ids the buttons had. Details toggles keep their open state in the page (a set of keys) so they survive rebuilds. The Knowledge source on/off becomes `setting_switch` (`knowledge-source-enabled-<kind>`).
Alternatives: hover-only actions (rejected: not reachable by keyboard and screen reader in the same way).

### D6. Tabs
A `TabButton` node in the Knowledge view draws the chosen tab with an accent underline and returns `selected_state()`; it replaces the standard/subtle button pair.
Alternatives: keep buttons and only set the accessible name (rejected: not visible enough, and no SelectionItem).

### D7. Deferred
Meeting error texts: the meeting service stores sentences, which the Thread progress line and tests rely on; codes need a migration of stored records and come in a later change. The captured-meetings list stays in Settings > Meetings: the meeting Thread now carries the facts (`improve-threads-calendar-agenda`), and moving transcript review into the Thread needs its own design.

## Risks / Trade-offs

- [Actions moved into menus are found less easily] -> Only the less frequent actions move; the menu button is named "More actions for <item>", and the main action stays visible.
- [A Tool without a name string shows its id] -> The fallback is the id, as today; names exist for the built-in Tools.
- [Hiding a Skill's source behind Details slows support] -> Details is one click away, and the Skill editor still shows the source.
