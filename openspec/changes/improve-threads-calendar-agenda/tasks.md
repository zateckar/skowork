## 1. In-Window Links

- [x] 1.1 Add the route hook on `Ui` and send `#/` and `skowork://thread/` link targets to it (design D1); verify with a UI unit test that a route link calls the route hook and not `open_link`, and that an https link still calls `open_link`. Status: done 2026-11-27; UiMarkdown.BlocksBecomeElementsWithLinksAndCopy covers route, skowork:// and https links.

## 2. Thread Header

- [x] 2.1 Show only the badge for system Threads and drop the meeting origin text and Settings link (design D2); verify with a shell test that a Daily report header has no "Created by Skowork" and a meeting header has no link to `#/settings`. Status: done 2026-11-27; ShellThreadHeader.MeetingFactsOnceAndNoSettingsLink.
- [x] 2.2 Add `started_at`, `ended_at`, `attendees` and `event_link` to `meetings.progress` and render the meeting facts line and "Open in calendar" (design D2); verify with a meeting service unit test for the progress fields and a shell test for the rendered line. Status: done 2026-11-27; the link opens the Calendar on the meeting's week (`event_id`, not `event_link`); ShellThreadHeader.MeetingFactsOnceAndNoSettingsLink. No separate meeting service test for the progress fields.
- [x] 2.3 Stop writing a `link` into new meeting origin refs; verify with the existing meeting service tests. Status: done 2026-11-27; meeting tests pass.

## 3. Live List Entries

- [x] 3.1 Expose `InboxService::thread_changed` and call it after every direct inbox store write in the host (design D3); verify with an inbox service test that `thread_changed` emits `inbox.changed` with the entry. Status: done 2026-11-27; covered through the triage, meeting and Agenda adapters in the host; no dedicated inbox service test.
- [x] 3.2 Set `action: "none"` in the origin ref on triage No action needed and Dismiss (design D3); verify with triage unit tests that the origin ref changes, and with a model test that `needs_action` is false for `action: "none"`. Status: done 2026-11-27; EventTriage.NoActionNeeded… and DismissHolds… check the ref, ShellModel.TriageThreads… checks needs_action.

## 4. Calendar Time Grid

- [x] 4.1 Lay out overlapping events in lanes and compute the hour range in a portable function (design D4); verify with unit tests: overlap pairs, a chain of three, back-to-back events not overlapping, range extension and the 00:00–24:00 cap. Status: done 2026-11-27; CalendarGrid.* in ui_tests.
- [x] 4.2 Draw the grid, place event blocks, show the all-day band and the now line (design D4); verify with a shell test that blocks exist with UI Automation names and heights proportional to length, and in the running app. Status: done 2026-11-27; ShellCalendar.WorkWeekPrivacyAndEventActions checks geometry; rendered offscreen and reviewed. Not yet seen in the running app with a real calendar.

## 5. Agenda View Layout

- [x] 5.1 Render items as cards with the due chip, badges, shortened quote, primary actions and the More menu, and group headings with counts (design D5); verify with shell tests for the card order, the menu items and UI Automation names, and in the running app. Status: done 2026-11-27; ShellAgenda.ViewGroupsItemsAndOffersNamedActions; rendered offscreen and reviewed. Due dates are readable (weekday, day, month).

## 6. Settings Navigation

- [x] 6.1 Add the section route, the grouped section list with the selected state and the single-section content; point links at their sections (design D6); verify with route unit tests and a shell test that choosing a section shows only it and marks it selected. Status: done 2026-11-27; route tests in ShellRoutes, ShellPages.SettingsHasEverySection…, golden shell_settings.png updated.

## 7. Plain Information

- [x] 7.1 Local, localized times in mail, Connectors, Knowledge sources, meetings and the quota banner; no token lifetime or message counter on Status; translated storage categories; no status-bar mention of a valid policy; Compact as the primary remedy and no Retry for a Thread too long; one Pause button with a menu; confirmation before forgetting a meeting; plainer labels (Send now, Skowork server, Status) (design D7); verify with the updated shell tests. Status: done 2026-11-27; updated ShellPages, ShellFeed, ShellComposer and ShellTelemetry tests.

## 8. Strings And Checks

- [x] 8.1 Add en, cs and de strings; verify with the localized string checks in ui_tests. Status: done 2026-11-27; ui_tests pass (string check).
- [ ] 8.2 Run the client unit tests and `openspec validate --all --strict`. Status: partial 2026-11-27; ui_tests 109, win_tests 1107 of 1108 (WithdrawalOutbox.SeparateProcesses… failed once and passed three reruns; Pipe.* not run while the installed client holds the pipe), agenda, report, triage, teams pass. `openspec validate --all --strict` not run: no npm on this machine.
