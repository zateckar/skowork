## 1. Review

- [x] 1.1 Render every screen offscreen with mocked host data and list the problems (proposal). Status: done 2026-11-27; 37 screens, Inbox views, Thread, Approval, new Thread, Status, 16 Settings sections, Telemetry, About, editors, Automation page, Knowledge, Apps, Agenda, Calendar.

## 2. Window

- [x] 2.1 Dates and times per moment in the fixed format, date-only values, grouped counts (D1); remaining raw/UTC times in Teams, Daily report, Triage, Knowledge, Apps, Chats, citations and list rows. Status: done 2026-11-27; ShellAgenda, ShellKnowledge.SearchResults… and the goldens check the format.
- [x] 2.2 Placeholders from labels; labels above the Automation editor, Skill editor and Daily report time fields (D2). Status: done 2026-11-27; rendered and reviewed.
- [x] 2.3 Tabs measured as drawn; wrapping checkbox labels (D3). Status: done 2026-11-27; golden shell_knowledge.png.
- [x] 2.4 Context indicator only on a Thread route. Status: done 2026-11-27; rendered and reviewed.
- [x] 2.5 Plain previews (D6). Status: done 2026-11-27; rendered and reviewed.
- [x] 2.6 Readable Approval arguments. Status: done 2026-11-27; golden shell_approval_card.png.
- [x] 2.7 Plain Settings wording: folder patterns, Policy sources, Skills Tools, Connectors, Status, Privacy, Agenda, Triage plurals, Knowledge counts, US spelling (D5). Status: done 2026-11-27; ShellPages.Skills…, .PolicyPage…, .Connectors…, golden shell_status.png.
- [x] 2.8 Settings row layout: removable rows, sub-heading spacing, Daily report time row and working days, captured meetings, capture sources, Automation rows (D4). Status: done 2026-11-27; golden shell_settings.png; rendered and reviewed.
- [x] 2.9 Apps cards with a More menu (D7). Status: done 2026-11-27; ShellApps.ListOpens… uses the menu.

## 3. Checks

- [x] 3.1 en, cs and de strings; verify with the build-time string check. Status: done 2026-11-27.
- [ ] 3.2 Run the client tests and `openspec validate --all --strict`. Status: partial 2026-11-27; ui_tests 114 pass; win_tests 1124 of 1126 (Pipe.* not run): WithdrawalOutbox.SeparateProcesses… fails as before, ShellDictation.DictateInserts… fails intermittently (1 of 3 runs) on the second dictation start, in code this change does not touch. Goldens shell_approval_card, shell_knowledge, shell_settings and shell_status updated. `openspec validate --all --strict` not run: no npm on this machine.
