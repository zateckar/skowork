# Design

## Context

- **Window:** add-client-foundation provides the on-demand WebView2 window and the embedded single-page app shell.
- **Data and events:** add-agent-runtime provides Threads, Runs, interrupts and steering, and stores them in `app.db`.
- **UX model:** this change re-specifies pizza-bot's inbox. The rewrite fixes the gaps we found: no Archive, no mark-unread, a per-tab running indicator, approvals without rationale (#111), and subagents that could not ask questions (#150).

## Goals / Non-Goals

**Goals:**
- Users can triage agent work the way they triage email, with no training.
- The list stays fast with 10,000 Threads: first paint under 300 ms, smooth scrolling.
- The UI holds no state of its own, so closing and reopening the window loses nothing.

**Non-Goals:**
- Mobile or web access.
- Multi-user shared Threads.
- Rich document editing inside the composer.

## Decisions

### D1. Thread state lives in the host
- **Choice:** `unread`, `archived`, `pinned` and `folder_id` are stored in `app.db`. The derived flags `has_action` and `running` are computed by the host from Run and interrupt state. The single-page app subscribes to change events over WebView2 messaging and renders view models.
- **Why:** the window is created on demand (client/desktop-host). Runs and Triggers keep running while it is closed, so the host must own the truth.
- **Alternative considered:** a UI-side store synchronised later. Rejected because it is what caused pizza-bot's per-tab running indicator.

### D2. List virtualisation and paging
- **Choice:** the host serves keyset-paginated list pages of 100 entries per view, and the single-page app virtualises rows.
- **Alternative considered:** loading every Thread. Rejected because 10,000 Threads would exceed the 300 ms first-paint target.

### D3. Search
- **Choice:** an SQLite FTS5 index over titles and message text in `app.db`, with the ICU/unicode61 tokenizer and diacritic folding for Czech and German.
- **Alternative considered:** reusing the knowledge index. Rejected because the knowledge layer is optional and arrives later (change 12), while inbox search is needed in the MVP.

### D4. Approval provenance
- **Choice:** client/agent-runtime gives every untrusted content span entering the context a provenance record (source kind, URI or item id, time), and requires a `rationale` on every call that can be decided approve. Each tool call records the provenance ids that were in its decision window. The Approval card shows that rationale together with the provenance list. The inbox only presents these records; it does not create them.
- **Alternatives considered:** no rationale, as in pizza-bot (users can't judge requests); or full claim verification, as in pizza-bot's draft PR #146 (deferred).

### D5. One Question card for every source
- **Choice:** `ask_user` Questions, client-raised Questions inside a Run (browser sign-in hand-over, computer-use Continue/Stop), and standalone Action items (connect mailbox, Remote task acceptance, meeting consent) all use the same Question card and view model `{question, choices[], allow_free_text, raised_by}`. The answer is routed back to the Run through client/agent-runtime, or to the capability named in `raised_by`.
- **Alternative considered:** a separate dialog for each feature. Rejected because it fragments the Action view and breaks keyboard triage.

### D6. Notifications
- **Choice:** Windows App Notifications through `ToastNotificationManager` (WinRT ABI, as in the toolchain spike), with the AUMID `Workbench.Client`. The host registers the AUMID per user under `HKCU\Software\Classes\AppUserModelId` as part of its per-user registration (client/desktop-host), so it is skipped with `--no-register`. The toast's launch argument carries the Thread id. The host process owns every toast it shows and handles a click through the toast's in-process `Activated` event, which opens the window at that Thread; the host is resident whenever a toast can exist, and it removes its toasts from the Action Center when it quits. Focus suppression uses the Thread the window reports as focused (D11).
- **Alternatives considered:** tray balloon tips (deprecated, no actions); a COM activator for clicks after the process ended (needs a Start-menu shortcut and `notificationactivationcallback.h`, which the toolchain lacks, and toasts never outlive the host).

### D7. Localization and accessibility
- **Superseded (2026-10-23)** by add-native-ui D8 and D9 (UI Automation, native string resources).
- **Choice:** typed TypeScript string bundles (cs, de, en) in the single-page app: one object per language with the same keys, parameterised strings as functions, plurals through `Intl.PluralRules`, and Intl APIs for dates and numbers. The type checker rejects a bundle with a missing key, and a unit test scans the components for hard-coded text. The language follows `navigator.language` (the Windows display language in WebView2) unless overridden in settings. Accessibility is verified with axe-core in the unit tests (components rendered in happy-dom) and a manual NVDA/Narrator pass for each release.
- **Alternative considered:** ICU MessageFormat bundles. Rejected for now: it adds a runtime parser dependency, while three languages with typed functions cover the plurals and parameters the UI needs.

### D8. Inbox data in `app.db` (migrations 300-399)
- **Threads:** new columns `origin_kind` (user, trigger, remote, meeting, fork, system), `origin_ref` (JSON link target), `unread`, `archived`, `pinned`, `folder_id`, `last_activity` and `preview`. The host updates them from the runtime's Run events: a `message` event refreshes the preview, the activity time and the search index; a Run start or a new Action item un-archives; a Run ending in succeeded or failed marks the Thread unread unless it is the focused Thread.
- **Folders:** a `folders` table. Deleting a folder clears `folder_id` on its Threads in the same transaction.
- **Views and paging:** each view is one query ordered by `(pinned DESC, last_activity DESC, id DESC)` with a keyset cursor over those three values and a page size of 100. The date group of an entry is computed by the single-page app from `last_activity`.
- **Titles:** the generated title is the first line of the first user message, at most 80 characters, so no model call is spent on it. It is set when the first Run of the Thread starts and can be renamed at any time.
- **Deletion:** `threads.delete` requires `confirm: true` and runs the forget cascade of client/local-storage for the entity type "thread". The inbox registers the handler that removes the Thread row, its Runs, events, Action items, queued messages, compactions, attachments, draft and search rows in one transaction.

### D9. Standalone Action items
- **Choice:** `action_items.run_id` becomes nullable and gains `raised_by`, the name of the client capability that raised the item. A capability calls `raise_standalone(capability, thread_id?, prompt, choices)` on the inbox service; without a Thread id, a Thread with origin system and the prompt as its title is created. The answer must be one of the fixed choices; it resolves the item and is passed to the handler that capability registered for its name. Standalone items count in the Action view, the tray and the Action notifications like Run items.
- **Test capability:** development builds expose `dev.raise_action`, which raises Connect / Not now for the capability `dev.test` and records the answers it receives.

### D10. Fork
- **Choice:** forking from message `(run_id, seq)` creates a Thread with origin fork and `origin_ref` `{thread_id, run_id, seq}`. Its history is one Run with role `fork` in state succeeded, whose event log is a copy of every `message` event of the source Thread's main Runs up to and including that message. Compacted history is copied as messages, not as the summary; the runtime compacts the fork again if needed.

### D11. Focus and read state
- **Choice:** the single-page app reports the Thread in focus with `inbox.focus` whenever the open Thread changes or the window gains or loses focus, and the host clears it when the window closes. Opening a Thread (`threads.open`) marks it read. Focus is held only in memory.

### D12. Composer drafts and attachments
- **Drafts:** a `drafts` table keyed by Thread id (`new` for the New Thread composer) holding the text, plus `draft_attachments` holding each attached file's name, size and content. Drafts are saved as the user types (debounced) and removed when sent.
- **Attachments:** the UI reads a picked, pasted or dropped file and sends its bytes to the host, at most 25 MB per file and 10 files per message. On send, documents (docx, xlsx, pptx, pdf) are converted to text by the sandboxed extract worker of client/core-tools; text files are kept as they are; other binary files are refused. Each one becomes a Thread attachment with untrusted file provenance, and the user message gets a line per attachment naming it and its attachment id, so the model reads it with `read_attachment`.
- **Skills:** the `/` list comes from `skills.list`. Until add-skills delivers the skill catalogue, it returns an empty list, and the composer says that no skills are available.

### D13. Feed and Activity rail view models
- **Choice:** the single-page app folds Run events into a feed view model in one pure function, used both for live `run.event` messages and for replaying stored events (`runs.events`) when a Thread opens. Subagent Runs are child Runs; entries started by tool calls of the same assistant message form one group in the Activity rail. A child's events are fetched only when its entry is expanded.

### D14. Testing the UI against a mock host
- **Superseded (2026-10-23)** by add-native-ui D10 (unit, golden-image and UI Automation tests of the native window).
- **Choice:** UI tests mount the whole single-page app in jsdom against an in-process mock host. The mock implements the same JSON-RPC methods and `run.event` / `inbox.changed` messages as the host, and it simulates Runs: Approvals, Questions, streaming, the six model-access errors, and automatic and manual compaction. The host side of each behaviour (Thread state, views, folders, search folding, feed snapshot, retry, fault injection, approval resumption) is tested in `win_tests` against the real runtime. The axe-core gate runs inside the same UI tests and fails on critical or serious findings. jsdom cannot compute colour contrast, so that check stays with the manual pass. The same mock also backs the dev server (`npm run dev`) for design work without the host.
- **Origin links:** trigger, remote and meeting origins link to the `history_url` or `link` in the origin reference when one is present. The changes that create those origins define the targets.

## Risks / Trade-offs

- [Users miss Action items while the window is closed] → Tray action-required state (client/desktop-host), toast notifications, and the Action count in the tray tooltip.
- [Notification fatigue from frequent Trigger Runs] → Per-kind toggles. Per-Trigger "notify on completion" is defined in client/scheduler.
- [Rationale text is model-generated and can be misleading] → Provenance is shown independently of the rationale, and the arguments are always shown in full.

## Open Questions

- Should the Activity rail also show non-delegation progress, such as plan checklists? This can be added later without changing the Thread model.
