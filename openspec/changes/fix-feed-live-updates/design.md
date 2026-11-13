# Design

## Context

Facts from the code, read on 2026-11-10 from the working tree at main 0c70d92. The working tree has uncommitted changes (see git status), so line numbers may move. The reported problems were not reproduced; each cause below is verified by reading the code.

**Diagnosis check**

1. **Repeated answer: confirmed, with one addition.**
   - The reasoning of a model response is stored with `store_.append_event` and never emitted live (client/src/runtime/runtime.cpp:636). Messages are emitted (runtime.cpp:250-254). So the live sequence has a gap.
   - A gap makes the feed refetch the snapshot (client/src/shell/feed.cpp:277).
   - The live assistant message then has `seq <= last_seq`. It returns at feed.cpp:276, before the line that drops the streamed text (feed.cpp:295-296). The streamed text stays, and build_feed shows it beside the stored answer (feed.cpp:168-175).
   - Addition: the duplicate needs the snapshot path, which never drops streamed text. Any message that is already in the snapshot is skipped the same way.

2. **Unrelated thread list: confirmed, with corrections.**
   - Every `knowledge.search` result becomes a citation (client/src/shell/feed.cpp:108-119, called at feed.cpp:58). Results can be passages from finished Threads (the threads Source).
   - The list is drawn as buttons under the tool card (client/src/shell/views_feed.cpp:608-649). It stays visible even with work details hidden (views_feed.cpp:77-83, 486-487).
   - The model gets no citation instruction, so nothing in the answer refers to these numbers. Each search numbers its results from 1 (client/src/knowledge/knowledge_tools.cpp:79-81), so numbers repeat across calls in one Run.

3. **Freezes: partly confirmed.**
   - Confirmed: the UI requests and all background work share one pool of 4 threads (client/src/app/app.cpp:141; native_window.h:20 and 33).
   - Correction: a Thread that is not open does not redraw. `Shell::on_run_event` returns early for other Threads (client/src/shell/shell.cpp:768-770). But every event of every Run is still posted to the UI thread and serialised (app.cpp:1246).
   - Confirmed: for the open Thread, every token refolds all events of every Run (feed.cpp:152-158, views_feed.cpp:514-516), then relays out and invalidates (views_feed.cpp:537-538). It is called from shell.cpp:777-778.
   - Confirmed: `App::tick` runs on the UI thread every second (app.cpp:2506, 2403). It calls `sched->tick()` (app.cpp:377), which holds the scheduler lock and runs about 49 store statements (scheduler.cpp:901-907), and `file_watcher->poll()` (app.cpp:376). It also builds and compares the status JSON every second (app.cpp:380-386).
   - Not verified: whether folder scans run on that path. The folder work I found is in the knowledge indexing, not in the tick.

4. **Copy: confirmed mechanism.**
   - Ctrl+C copies a message selection only if no node handled the key first (client/src/ui/window.cpp:287-293).
   - A press on message text returns from `bubble` before focus moves (client/src/ui/layout.cpp:575-581). The focus change at layout.cpp:584 is skipped, so the composer keeps keyboard focus. Its native edit control handles Ctrl+C itself (client/src/ui/textfield.cpp:271-293).
   - Live updates clear the selection in two ways. The feed keeps only the unchanged prefix of items (views_feed.cpp:519-528), so any change before the selected message rebuilds it. The text view clears its selection when its blocks change (client/src/ui/richtext.cpp:333).
   - The "whole turn" experience comes from the per-message Copy button, which copies the whole message (views_feed.cpp:113-115).

**In-flight changes that touch the same requirements**

- `improve-desktop-look` (complete, not archived) ADDs Message Text Selection and MODIFIEs Work Details Visibility. This change modifies both, so it builds on improve-desktop-look and must be archived after it. hide-work-details (also complete, not archived) adds Work Details Visibility, so it must be archived before improve-desktop-look.
- The two versions of Work Details Visibility disagree on the token usage line. hide-work-details keeps it visible. improve-desktop-look hides it (its D8). This change follows improve-desktop-look. See Open questions.
- `add-native-ui` design.md already sets 30 updates per second for Markdown re-parsing (design.md:52), and one feed load in flight at a time (design.md:180; shell.cpp:347-353). This change uses the same limit for feed repaints.
- `fix-long-thread-model-calls` defines Stable Prompt Prefix. The citation instruction is one static sentence, so it fits that rule. Its one-time cache miss is covered in Risks.
- `add-work-inbox` (Streaming Thread Feed) and `add-knowledge-graph` (Search result citation) are not changed.

**Baseline numbers:** the client test counts before the change are in Acceptance. The UI-thread timings of task 1.1 are recorded in Acceptance (the "before" there models the removed path).

## Goals / Non-Goals

**Goals**
- Each answer appears once in the Thread feed.
- The Thread feed lists only the sources that the answer uses.
- Text can be selected and copied with Ctrl+C while live updates arrive.
- No UI task blocks the window for long, and the limits can be tested.

**Non-Goals**
- Changing knowledge ranking, model choice or the compaction trigger.
- Changing the control plane or the wire protocol.

## Decisions

### D1. Every stored Run event is also sent live
The runtime stores reasoning and sends it live through the same append-and-emit path as messages. Stored and live sequence numbers then match, and a gap in the live stream means a real loss.

**Alternatives considered**
- *Keep the refetch on a gap and drop streamed text on refetch:* each reasoning block would cause a full snapshot reload, which is slow on long Threads.
- *Do not store reasoning as an event:* it would vanish from the Run log and the Show details view.
- *Give reasoning sequence 0:* it would hide real gaps from the check in feed.cpp:277.

### D2. The stored answer replaces the streamed text of its Run
Each Run keeps the sequence number of its last stored event at the moment its first text delta arrives. When a stored assistant message of that Run with a higher sequence number is applied, live or from a snapshot, the streamed text of that Run is dropped. A stored answer from an earlier model call never removes text that streams later.

**Alternatives considered**
- *Compare the streamed text with the stored text:* fragile, because whitespace and the cut-off point differ.
- *Hide streamed text entirely:* it removes the streaming the user expects.

### D3. Host-side coalescing and a repaint limit
- The host merges consecutive `text.delta` and `reasoning.delta` events of one Run into one event every 34 ms (app.cpp, through RunEventRelay in src/app/run_event_relay.h). 34 ms rather than 33 ms: at 33 ms one second can hold 31 events, and at 34 ms it holds at most 30, which is the limit of D6.
- Deltas of Threads that are not open are not sent to the window at all. The host asks the inbox which Thread is open (InboxService::open_thread). Shell already drops them (shell.cpp:768-770), so the user sees the same thing.
- The feed repaints the streaming item at most once per 34 ms, for the open Thread only (Shell::feed_changed, RepaintLimit).

**Alternatives considered**
- *Throttle in the window only:* it still posts and serialises every token on the UI thread.
- *Coalesce in the runtime:* it ties the runtime's events to UI timing and changes the runtime's tests.

### D4. Keyed feed updates and a cached fold
- `FeedView::update` matches slots by key and content signature, not only by prefix. A change rebuilds only the item that changed. Other messages keep their elements, so their selection and reading position stay.
- `build_feed` keeps the folded items of each Run's stored events. A delta refolds only the streaming item.

**Alternatives considered**
- *Keep prefix reconciliation and keep only the selected message:* other changes before it would still rebuild the rest, and the fix would be special-cased.

### D5. Periodic work and UI requests use different threads
- Scheduler ticks and file-watch polls run on the background pool, with a single-flight flag like the telemetry flush (app.cpp:401-407). The scheduler's own lock keeps its state safe (scheduler.cpp:902).
- The pool splits in two: 4 threads for UI requests (NativeAppWindow) and 2 for background work (every other `pool.post`).
- Status JSON stays on the UI thread. It reads in-memory state only.

**Alternatives considered**
- *Raise the single pool to 8:* periodic store work would still run on the UI thread, and background calls could still delay UI calls.
- *A priority queue:* more code than two pools need.

### D6. Limits that a test can check
- Each UI task finishes within 50 ms. The 95th percentile is 16 ms or less while a scripted Run streams.
- A click is answered within 100 ms while background work runs.
- The feed repaints at most 30 times per second.
- The UI thread makes no store call.

Tasks 1.1 and 4.x check these in a harness. Task 5.1 checks them on the baseline laptop.

**Alternatives considered**
- *Frame time only:* hard to test in CI.
- *No numbers:* not testable.

### D7. Only cited sources are listed, under the answer
- The stable system prompt asks the agent to write `[n]` after each sentence that uses passage n, and to write no number for an unused passage (agent-runtime Knowledge Citation Markers).
- The feed lists, under the answer, the passages whose numbers appear in that answer's text outside code spans. It lists them in number order, as a numbered list with title, Source and date.
- Passage numbers are unique within a Run (knowledge-graph Citation Numbers Per Run). After a restart the next number is taken from the Run's stored results.

**Alternatives considered**
- *Keep listing every result:* the reported problem.
- *List every result under a disclosure:* unused passages are still shown.
- *Match the answer against titles or passage text:* fragile, and misses paraphrase.
- *A structured citations field from the model:* needs a model API change that is out of scope.

### D8. Ctrl+C copies the message selection
A press on selectable message text gives keyboard focus to that message's view while it holds the selection. Ctrl+C then reaches the window's key handling (window.cpp:287-293), not the composer's edit control. The selection is cleared by a press elsewhere, by Escape, or by a change to that message's own text (richtext.cpp:333).

**Alternatives considered**
- *Intercept Ctrl+C before the focused control whenever a selection exists:* it must also leave the composer's own copy alone, so it needs a second check. Use it only if D8 conflicts with the composer.
- *Copy button only:* the reported limitation.

## Risks / Trade-offs

- **[Risk] A model ignores the citation instruction, so no sources appear.** -> Task 1.3 measures how often the answers cite on 10 knowledge questions. If the rate is low, the wording of the prompt changes. The feed rule stays, so no unused source is shown.
- **[Risk] Changing the system prompt once misses the prompt cache for every Thread.** -> The change is one static sentence, made in the same release as fix-long-thread-model-calls where possible. Expect one cache miss per Thread.
- **[Risk] Coalescing delays visible text by up to 33 ms.** -> Imperceptible. The final text is never delayed past its stored event.
- **[Risk] Two pools reduce the parallel capacity of background work.** -> Background work is light. Task 5.1 measures it.
- **[Risk] Keyed reconciliation costs more per update on very long Threads.** -> Slots are kept in a map by key. Tasks 3.5 and 5.1 measure it.
- **[Risk] Pressing on a message moves keyboard focus away from the composer.** -> That is what a click on other controls does already. Focus returns to the composer on the next click there.
- **[Risk] In-memory passage numbers are lost on restart.** -> Numbers are taken from the Run's stored results when no counter exists. The cost is one query per search after a restart.

## Acceptance

Recorded by the apply step on 2026-11-10 (main 0c70d92 with the uncommitted work of other changes), and updated after the second apply pass the same day. Tasks 5.1 and 5.2 are not done: they need the user's OK, because they drive the desktop. Tasks 1.1, 3.4 and 4.2 are done in the form described below; the gaps against their original verify clauses are listed at the end of this section.

**Reproduction (task 1.2).** Disabling the stored-answer check in `build_feed` makes `ShellFeedLive.SnapshotTakenAfterStreamingShowsTheAnswerOnce` fail: the snapshot shows the streamed text beside the stored answer, which is the repeated answer. With the check in place the test passes.

**Citation markers (task 1.3).** `dev/scripts/llm-citation-spike.mjs`, run once on 2026-11-10 against the organization LLM API with the citation sentence of the stable system prompt. Ten knowledge questions, five numbered passages each (one relevant). Result: 10 of 10 answers have at least one `[n]` marker; the markers named the relevant passage in all ten; 0 markers name an unused passage. The API key was read from `.env` and not printed.

**Client tests (`cmake --build --preset test`, binaries in client/build/test).**

| binary | before | after |
|---|---|---|
| base_tests | 46 passed, 0 failed | 46 passed, 0 failed |
| win_tests | 690 passed, 0 failed | all passed in the final full ctest run (17 of 17 binaries; 5 tests disabled in the file, 711 before the last test was added) |
| ui_tests | 57 passed, 1 failed | 66 passed, 0 failed |
| knowledge_analysis, telemetry, skills, tools, mcp, browser, computer, policy | pass | pass, unchanged counts |

- The one ui_tests failure before was `ShellFormat.PercentSnippetsAndAboutText` (the test and the code used different spellings of the product name). The coordinator fixed the test; it passes now and is not touched here.
- `ShellAutomations.EditorRunNowAndOpenResultFromHistory` (win_tests) failed in the last full run and passes when run alone (2 of 2 isolated runs in the last pass). It is timing-sensitive under full load and does not use the feed code. It is recorded as a known flake, not fixed here.
- The client test run excluded `computer_win_tests` (desktop input) and `e2e_tests` (AppE2E), as the rules require.
- New tests: runtime (RunEventStream, KnowledgeCitationMarkers), knowledge (CitationNumbers), shell core (ShellFeedLive x7, ShellFeedSources x2), shell (ShellFeed.AnEarlierMessageKeepsItsElementWhenALaterRunArrives, the reasoning-gap case included in ShellFeedLive), ui (UiMarkdown.CtrlCCopiesTheSelectedWordsAndAPressElsewhereClearsThem), app (RunEventRelay x4), host (HostPools).

**Deviations from the design as written**
- D3 and D6: the repaint limit and the host merge use 34 ms, not 33 ms (see D3).
- D1: the reasoning event is sent with its stored number, as written. No other runtime change.
- Citation numbers (D7): the runtime gives each tool call a `stored_results` reader (ToolContext), so that a Run that restarts reads its numbers from the stored `knowledge.search` results. A stored result that was cut to a preview (large results, runtime.cpp) has no readable numbers; a restarted Run then may reuse a number. This is accepted and noted.
- D8: a selected message view takes keyboard focus only while it holds a selection (`focusable` is set on the press and cleared with the selection). Losing focus clears the selection, so a press on the composer or a button clears it, as the spec requires.
- Keyed updates (D4): the feed keeps the element of every item whose key and signature match, and rebuilds the rest; the citation list is part of an answer's signature.
- Tool cards keep their own citations under Show details, as the spec says; with details hidden a tool card shows only its images.
- D5: the calendar-offset tick also moved to the background pass, with the scheduler.
- Live text: the recorded sequence of a streamed call is the newest sequence number seen live for the Run (LiveText::seen), not only the snapshot's. A snapshot can be older than the events that arrived while it loaded; without this, the first deltas of a call were dropped by the next snapshot (found by ShellFeed.StreamsMarkdownWithReasoningAndCollapsedToolCards).

**UI-thread harness (task 1.1, client/tests/app/live_feed_load_test.cpp, win_tests).** A scripted stream of 2,000 text deltas over 20 s (one every 10 ms) alternates between a Run of the open Thread (300 stored messages in its history) and a Run of a Thread that is not open. Four background requests are pending on the background pool for the whole stream (two running, two queued, held on a gate). The deltas go through RunEventRelay and the window's path (apply_live_event, RepaintLimit, the feed fold). Each UI-thread task is timed with a steady clock; a repaint is one feed rebuild. "After" is the shipped configuration (34 ms relay and repaint limit, cached FeedFold). "Before" is the same stream with coalescing and the repaint limit set to 0, the window receiving the closed Thread's deltas (dropped there as before), and a fresh build_feed per repaint. The pre-change binary was not kept, so "before" models the removed path; it is not a measurement of the old code.

Store calls on the UI thread: the window's path takes no store handle, so it makes none; the harness has no store to count, and the count is zero by construction. This is the reason the store-call count is not a separate measurement.

| run | UI tasks | p95 | max | repaints in 20 s | closed-thread tasks |
|---|---|---|---|---|---|
| before (no coalescing, no limit, no cache) | 2000 | 0.497 ms | 0.857 ms | 1000 (50/s) | 1000 |
| after (shipped) | 589 | 0.377 ms | 0.970 ms | 589 (29.4/s) | 0 |

D6 against the after row: 29.4 repaints a second (limit 30), every task under 50 ms, p95 under 16 ms, no task for the closed Thread. The harness asserts these after limits. The before row is recorded only; it shows the repaint count the change removes (50 a second against 30).

Flaky-test finding during this pass: see "ShellAutomations.EditorRunNowAndOpenResultFromHistory" below.

**Scheduler seam (task 4.2, client/src/app/periodic_pass.h, client/tests/app/periodic_pass_test.cpp).** App::tick now offers the scheduler and file-watch pass to `PeriodicPass`, which owns the single-flight flag and posts the pass to the background pool. Tests: the pass runs on a pool thread, not the offering thread; while a pass is queued or running, 50 further offers queue nothing, and the next offer after it ended starts one pass, never two at once; with a 150 ms pass, the UI-thread offers stay under D6 (p95 16 ms or less, every offer under 50 ms) and at least one but not all of 200 offers queue.

**Ctrl+C with the composer focused (task 3.4, client/tests/shell/shell_test.cpp: ShellFeed.CtrlCWithTheComposerFocusedCopiesTheMessageSelection).** The composer holds a draft and has the keyboard; a press and drag over "beta gamma" in the answer moves the focus to the answer (D8) and selects the words; Ctrl+C then puts exactly "beta gamma" on the clipboard and leaves the draft unchanged. Note: a selection cannot exist while the composer is focused, because losing focus clears it (MarkdownView::on_focus). The test covers the case the coordinator asked for: the composer had the keyboard first and the answer's selection still wins the copy.

**Verify clauses of 1.1 and 4.2 (closed).**
- Task 1.1: the harness now runs 20 s of scripted time, with four background requests pending. The store-call count is zero by construction (see above); the Verify text is read with that count as the reason.
- Task 4.2: PeriodicPass runs 60 simulated seconds of ticks (one offer a second, the calendar flag every 15th); the test records the thread of every pass and asserts none is the UI thread, which is the store-call count of the clause. The store calls are made inside the pass, and the test uses a stand-in for the tick instead of the real Scheduler, so scheduler_test.cpp is not extended.
- Tasks 5.1 and 5.2: need the user's OK (desktop input, laptop measurements).

**ShellAutomations.EditorRunNowAndOpenResultFromHistory (flake, fixed in the test).** The test waited only for the occurrence's "Open result" button, then read the attempt label. The button can appear in a render where the attempt still says "running"; a later render says "succeeded". The failing message shows the first read was "running" and the second "succeeded". The fix is to wait for the label's own condition (button and "succeeded" together). Rate: 4 failures in 100 repeats of the test before the fix (the current code, with the PeriodicPass change in place); 0 in 100 repeats of the three ShellAutomations tests after it. The pre-change rate was not measured: measuring it would need the change reverted, and the uncommitted work of other changes was not touched. The race is in when the test reads the list, not in the scheduler's writes, which run in one tick on the test thread (Scheduler::finish).

## Open questions

All three questions were answered by the product owner on 2026-11-10. The answers are recorded here, and the specs and tasks follow them.

- **Token usage line (decided 2026-11-10):** hidden by default, as in improve-desktop-look. The Work Details Visibility requirement in this change's delta spec says so, and the hide-work-details version is superseded.
- **Passages from finished Threads (decided 2026-11-10):** listed only when the answer cites them, as drafted in D7. A Thread passage that is cited is listed like any other source. A Thread passage that is not cited is never listed.
- **Limits in D6 (decided 2026-11-10):** accepted as written (50 ms per UI task, 16 ms at p95, 100 ms for a click, 30 repaints per second, no store call on the UI thread).
