# Design

## Context

- **Current state:** add-client-foundation D4 chose a WebView2 single-page app (Svelte and TypeScript), and add-work-inbox built the inbox in it. The host side is plain C++: view-model services expose JSON-RPC methods (`threads.*`, `runs.*`, `inbox.*`, `settings.*`) and push events (`run.event`, `inbox.changed`). The single-page app only renders them.
- **Decision (2026-10-23):** the product owner rejected the web UI. The client is meant to be lean and fast, and a browser engine in the window works against that: several extra processes and a second technology stack. The UI is to be native Win32 with Direct2D.
- **What stays:** every host service, the JSON-RPC methods, the named pipe API, and all client/work-inbox behaviour.

## Goals / Non-Goals

**Goals:**
- One process, one language: the window is drawn by `Workbench.exe` in C++.
- Window budgets that are measured (client/desktop-host Window Footprint): at most 150 MB in the core process with a 200-message Thread open, first content within 300 ms, 60 frames per second when scrolling, rendered in software (D1).
- The whole client/work-inbox spec, including WCAG 2.1 AA accessibility, Czech, German and English, and keyboard operation.

**Non-Goals:**
- A general-purpose UI framework. The toolkit has the widgets Workbench needs and nothing more.
- Pixel-identical reproduction of the current web UI's look.
- Animations beyond simple fades and expand or collapse.

## Decisions

### D1. Rendering stack
- **Choice:** Direct2D 1.1 rendering in software: a Direct3D 11 WARP device (no GPU driver is loaded) with a DXGI flip-model swap chain. DirectWrite for all text and WIC for image decoding. Per-monitor DPI awareness v2. (Revised 2026-10-24 after the spike, see Validation Results; the first version used the hardware device.)
- **Why:** these are the Windows-native 2D and text APIs. They ship with Windows, need no runtime, and work with GCC through the MinGW-w64 headers. DirectWrite handles complex scripts, font fallback, colour emoji and ClearType.
- **Why software:** the spike measured the same CPU time per frame on both paths (about 2–3 ms; the work is text layout and glyph rasterisation), and both held 60 fps. The hardware device cost about 40 MB more with the window open and kept about 20 MB of private memory after the window closed, against the 60 MB idle budget. WARP also cannot hit GPU driver faults or device removal on a docking or driver update.
- **Alternatives considered:**
  - Direct2D on the hardware Direct3D device: measured; heavier, not faster for this UI.
  - GDI with DirectWrite text through its GDI interop: no Direct3D at all, but shapes without anti-aliasing and no premultiplied-alpha composition; not measured, because software Direct2D already meets the budgets with one drawing API.
  - Keep WebView2: rejected by the product owner (Context).
  - WinUI 3: needs the Windows App SDK runtime and MSVC-centred tooling.
  - Qt: licence cost and about 20 MB of runtime.
  - Win32 common controls only (owner-drawn ListView and similar): accessibility comes for free, but a streaming rich chat feed with cards and Markdown is impractical in them.
  - Immediate-mode toolkits such as Dear ImGui: no accessibility tree, no native text input or IME.
  - GDI/GDI+ alone: poor text quality at high DPI and no colour emoji.

### D2. A small retained-mode toolkit
- **Choice:** an in-house element tree under `client/src/ui/`:
  - elements: box, text, rich text (Markdown), image, button, icon button, checkbox, menu, scroll view, virtual list, splitter, tooltip, badge, card;
  - layout: row and column stacks with fixed, auto and fill sizes, padding and gaps;
  - invalidation of dirty rectangles; hit testing; a focus chain with Tab and arrow keys; text-layout caches keyed by text and width.
- **Why:** the screens of client/work-inbox are lists, cards, text and a few controls. A tree that keeps state between frames allows caching and virtualisation, which the 10,000-Thread list needs. UI Automation (D8) maps one provider to one element.
- **Alternative considered:** drawing each screen directly in WM_PAINT handlers. Rejected because focus, hit testing and accessibility would be repeated on every screen.

### D3. Text input through rich-edit controls
- **Choice:** the composer, search and settings text fields are native rich-edit child windows (Msftedit `RICHEDIT50W`), with the system's spell checking enabled.
- **Why:** IME, dictation, spell check, clipboard, undo, right-to-left text and the accessibility of text input come from Windows. Re-implementing a text editor is the most error-prone part of any custom toolkit.
- **Alternative considered:** a custom-drawn editor with Text Services Framework. Rejected for effort and risk.

### D4. Markdown
- **Choice:** vendored md4c (C, MIT licence, pinned source, built by CMake) parses model output into blocks and inline spans. The toolkit renders paragraphs and headings as DirectWrite layouts with formatting ranges, lists with markers, tables as grids, and code blocks in a monospace font (Cascadia Mono, then Consolas) with a Copy button. Links show their URL on hover and open in the default browser with `ShellExecuteW` on click. Image syntax renders as alternative text plus the link, and raw HTML as literal text (client/work-inbox Markdown Rendering).
- **Streaming:** only the message that is streaming is parsed again on each delta, at most 30 times per second. Finished messages keep their layouts.
- **Why md4c:** small, fast, CommonMark-compliant with the GitHub extensions, and a callback parser that allocates little. It is fuzzed as an untrusted-input parser.
- **Alternatives considered:** cmark-gfm (larger, builds a full tree first); an in-house parser (CommonMark's edge cases are costly to get right).

### D5. The window uses the host's services in-process
- **Choice:** the window calls the same JSON-RPC methods through the in-process RPC registry (exposure `ui`), with no serialisation boundary beyond the existing JSON values. Host events (`run.event`, `inbox.changed`) are queued and delivered to the UI thread with a posted window message. The feed fold of add-work-inbox D13 is ported to C++ as a pure function and unit-tested.
- **Why:** the services, the pipe API and their tests stay as they are. Only the presentation is replaced.
- **Alternative considered:** calling the runtime's C++ classes directly from the views. Rejected because it couples views to internals that the pipe API already abstracts.

### D6. Threading
- **Choice:** the main thread runs the message loop, layout and drawing, as it does today for the tray. Text-layout creation for a newly loaded page of messages happens on the UI thread, in slices of at most 8 ms. Image decoding runs on a worker thread.
- **Alternative considered:** a separate render thread. Not needed at these frame budgets, and it would add locking to every element.

### D7. Look, theme and DPI
- **Choice:** Segoe UI Variable (Segoe UI as fallback); light and dark following the Windows app mode; high-contrast themes use system colours only and turn off custom colours and shadows; all sizes in device-independent pixels, scaled per monitor.
- **Why:** it follows Windows instead of inventing a style, and the high-contrast behaviour is required by client/work-inbox Accessibility.

### D8. Accessibility through UI Automation
- **Choice:** each focusable or meaningful element has a UI Automation fragment provider (Invoke, Toggle, ExpandCollapse, Selection, Scroll, Value and Text patterns as fitting), returned from `WM_GETOBJECT`. New Action items raise a UIA notification event, so screen readers announce "Approval required: <tool>". Rich-edit controls bring their own providers.
- **Verification:** UIA-driven tests in win_tests walk the tree and check every control's role, name and keyboard reachability, and the release checklist includes a manual pass with NVDA and Narrator. This replaces the axe-core gate (add-work-inbox D7, task 7.2).
- **Alternative considered:** MSAA (`IAccessible`). Legacy, with poorer support in Narrator.

### D9. Localization
- **Choice:** one string source per language (cs, de, en) as JSON under `client/src/ui/strings/`, compiled into the client as generated source (a resource in a static library is dropped by the linker unless referenced). A build step fails when a language lacks a key, a parameter or a plural form; English is the reference bundle. Plurals follow hand-written rules for the three languages (Czech one, few, other; German and English one, other). Dates and numbers use `GetDateFormatEx` and `GetNumberFormatEx` with the chosen locale. The language follows the Windows display language unless overridden in settings.
- **Alternative considered:** Win32 STRINGTABLE resources. No parameters or plurals, and editing .rc files by hand per language is error-prone.

### D10. Testing
- **Choice:** unit tests for layout, Markdown rendering, the feed fold and the view models in win_tests. Golden-image tests render screens to an off-screen Direct2D target on the same WARP device the window uses (D1), read the pixels back and compare against reference images with a tolerance. UIA tests (D8) drive the real window. The host services keep their existing tests.
- **Replaces:** the jsdom UI tests against a mock host and the UI mock dev server (add-work-inbox D14).

### D11. Removal and Microsoft sign-in
- **Choice:** after the native window passes the client/work-inbox spec, remove `client/ui/`, the UI packer and resource, `WebView2Loader.dll` and the WebView2 header. add-mail-calendar's interactive Microsoft sign-in uses the system browser with a loopback redirect, the same pattern as client/identity, instead of a hosted WebView2 window.
- **Why the sign-in moves:** keeping WebView2 for one sign-in window would keep the dependency, and RFC 8252 already recommends the system browser for native apps.

### D12. Staging
- **Choice:** build the native window behind a development flag while the WebView2 window keeps shipping, in this order:
  1. the shell: window, three panes, theme and DPI;
  2. the Thread list and the views;
  3. the feed with Markdown and tool cards;
  4. the composer;
  5. Approval and Question cards;
  6. the Activity rail;
  7. search, status bar and settings.

  The switch happens when the whole client/work-inbox spec passes on the native window. The web UI is removed in the same release.
- **Why:** the client stays usable at every step, and each step is testable on its own.

### D13. Screens
- **Choice:** the screens live in `client/src/shell/`, split in two:
  - `wb_shell_core`, portable: the view models ported from the web UI with their tests. These are the feed fold (D5), the Thread list order, views and date groups, routes, the shortcut catalogue, the status derivations (banners, connection state, MCP health) and formatting.
  - `wb_shell`, Windows: `MainWindow` and its views on the toolkit.
- **Interface:** `MainWindow` keeps the WebView2 window's interface, so the host does not change:
  - methods `open(route)`, `close()`, `is_open()` and `emit(event, data)`;
  - the same route strings (`#/inbox/...`, `#/threads/<id>`, `#/status`, ...);
  - the same host events.

  Both windows implemented `AppWindow` while the switch was staged, and the app picked the native one with the development flag `--native-ui`. Task 4.2 removed the WebView2 window and the flag, so the native window is the only implementation.
- **Host calls:** a `HostLink` reaches the host. In the app it runs RPC handlers on the thread pool and posts their replies to the main thread. The tests use the same link over a real `InboxService` and the scripted runtime. Replies that arrive after the window closed are dropped.
- **Lifetime:** the window and all its state exist only while it is open, as the On-Demand UI Window requirement asks. Reopening returns to the last route.
- **Responsive layout:** the web UI's breakpoints are applied at the start of every layout pass (`Tree::on_layout`), before anything is measured:

  | Window width (DIPs) | Layout |
  |---|---|
  | below 720 | navigation shows icons only (labels stay as accessible names) |
  | below 860 | one pane: either the Thread list or the open Thread, with a Back button |
  | 860 to 1179 | the Activity rail is a drawer over the content; a press outside closes it |
  | 1180 and wider | the rail is docked |

  Ctrl+B shows or hides the rail in every layout.
- **Dialogs:** dialogs (rename, new folder, delete) are modal toolkit dialogs. Text fields are rich-edit child windows (D3), shown only while focused or filled, so a dialog or menu drawn by the window is never covered by an idle field.

## Footprint Estimate

- **Code:** the toolkit is about 12,000 lines and the screens about 10,000 lines of C++. md4c is about 7,000 lines of C. Together they are expected to add 1.5 to 2 MB to `Workbench.exe`.
- **Install size:** the install shrinks by the embedded UI resource (about 2 MB) and `WebView2Loader.dll` (0.2 MB).
- **Memory with the window open:** measured 68 MB in the core process with software rendering (Validation Results), from the swap chain, glyph caches and text layouts. Today the WebView2 processes alone typically use 150 MB or more.

## Validation Results (2026-10-24)

Spike `client/spikes/nativeui` on a development workstation, not the baseline laptop: Intel Core i7-12700 with its integrated Intel UHD Graphics 770, 120 DPI, 60 Hz. Release build, GCC 16.2. The window shows 10,000 synthetic Threads and 200 Markdown messages (tables, lists, code, links, Czech text, emoji), with off-screen layouts evicted.

| Measure | Hardware Direct3D device | WARP (software) |
|---|---|---|
| First content after the window is opened (warm) | 83–92 ms (device 29 ms) | 59 ms (device 4 ms) |
| First content, first launch after build (cold) | 268 ms | — |
| List scroll, 600 frames at 40 DIP per frame | 60 fps, 0–1 frame over 1.5× the period | 60 fps, 0 over |
| Feed scroll top to bottom, 600 frames | 60 fps, 0–2 frames over | 60 fps, 0 over |
| CPU per frame (p50 / p99) | about 2.1 ms (p99 includes waiting for the previous frame at latency 1) | 2.5 / 3.4 ms |
| Streaming 4,000 characters in 200 deltas: parse and layout per delta (p50 / max) | 0.8 / 1.9 ms | 0.9 / 2.2 ms |
| Composer: typed character until repainted (p50 / max) | 1.4 / 2.8 ms | 1.4 / 2.9 ms |
| Working set after 60 s with the window open | 96 MB | 68 MB |
| Working set by stage: before device / after device / first content | 17 / 56 / 91 MB | 17 / 20 / 44 MB |
| After closing the window: working set / private bytes | 58 / 40 MB | 34 / 19 MB |
| Child processes | 0 | 0 |

Findings:
- Both paths meet every Window Footprint budget on this machine. The CPU work per frame is the same: the GPU does not make this UI faster, because the work is text layout and glyph rasterisation, not fill rate.
- The hardware device costs about 40 MB when the window opens and keeps about 20 MB of private memory after it closes (driver heaps and caches), which counts against the 60 MB idle budget.
- Text layouts must not hold device brushes beyond the device's lifetime: a first version kept the device alive after close through drawing-effect brushes (fixed: layouts are evicted before the device is released).
- UI Automation (probe `nativeui-uiaprobe`): 45 elements, all named, with correct roles; Invoke, ExpandCollapse and SelectionItem work; the approval's notification event arrives with its text. The composer is exposed as "RichEdit Control" and needs an accessible name. The Tab-order check could not run because the spike window was not the foreground window; it is left to the manual pass.
- Composer (rich edit): Czech text, emoji surrogate pairs, Unicode paste, undo, spell checking and Text Services Framework work. IME is still to be checked by hand.

Toolkit findings (tasks 2.3–2.5):
- Device resources belong to the renderer, not to a thread. Brushes are cached on the renderer and dropped with its device; image bitmaps are keyed by a process-unique device id. A first version kept brushes in a thread-local cache: when a window's thread exited, the device's last references were released inside the thread-exit callback, under the loader lock, and the next WARP device creation in the process hung. Keying by the device context's address was also wrong, because a later device can reuse the address.
- Menus and tooltips are floating children of an overlay layer above the content. A press outside them or Escape closes them and returns keyboard focus to where it was, found again by the element's runtime id.
- The Markdown view rebuilds only the blocks after the last unchanged one, so a streaming message keeps the layouts of its finished paragraphs. A link opens only after a confirmation that shows its URL, and only for http, https and mailto.
- UI Automation calls the providers on its own threads, not on the window thread. An earlier version assumed the window thread and read and changed the element tree from those threads. The add-save-as-note acceptance, driven through UI Automation, crashed the client in rebuilds and in painting. Each provider call that touches the tree is now sent to the window and runs on its thread. `UiAutomation.PatternsActOnTheElements` checks that Invoke and Select handlers run on the window thread, and it fails without the fix. A UIA client in the window's own process deadlocks when that thread raises an event. So the tests split the work: in-process clients check roles, names and patterns on a window that is never shown, so it raises no focus events, and the notification-event test listens from a second process, as a screen reader does. The product has no in-process client.
- Golden images (`client/tests/ui/golden/`) are drawn with Segoe UI Variable and Segoe Fluent Icons, and the tests skip on machines without those fonts. `WORKBENCH_UPDATE_GOLDEN=1` rewrites the images.

Product measurement (task 4.1, 2026-10-24, the same development workstation, not the baseline laptop): `ShellFootprint.WindowOnAThreadOf200Messages` in `win_tests` (release build, `WORKBENCH_MEASURE=1`). It shows the real window, with WARP, at 1600 DIPs wide, on a Thread of 200 Markdown messages (headings, lists, a table, code, Czech text, emoji) with 2,000 Threads in the list. The process also holds the inbox service and the scripted runtime, as the core process does. Rerun it on the baseline laptop to close task 4.1.

| Budget | Measured | Limit |
|---|---|---|
| Working set after 60 s open | 91.4 MB (25.4 MB before the window opened) | 150 MB |
| First content after opening | 171 ms on first open, 80 ms on reopening | 300 ms |
| Feed scroll, 600 frames | p50 16.67 ms, p99 17.1 ms, max 19.8 ms; no frame over 25 ms | 60 fps |
| Thread list scroll, 600 frames | p50 16.66 ms, p99 17.0 ms, max 17.2 ms; no frame over 25 ms | 60 fps |
| Typed character until repainted | p50 1.3 ms, p99 6.2 ms, max 8.9 ms | 16 ms |

Frame times include waiting for the display's 60 Hz refresh, so about 16.7 ms is a frame on time; a frame over 25 ms (1.5 periods) is a dropped frame. After closing, the working set is 50.4 MB, about 25 MB above what it was before the window opened (allocator pages and caches the process keeps); that is still within the 60 MB idle budget.

Screen findings (group 3):
- The window's coordinate calls must run per-monitor DPI aware, as the app's manifest declares. In a process that is not DPI-aware (the test runner), `GetClientRect` outside the window procedure returns scaled values, and the layout came out 20% too narrow at 120 DPI. The tests therefore make the window thread per-monitor aware. The window now takes its initial DPI from the monitor rather than from the system value, which reads 96 in such a process.
- The Thread list's rows point into a copy of the entries that the list owns, so a live update that replaces the shell's entries cannot leave the rows pointing at freed memory.
- A structure change is laid out from a posted window message as well as at paint time. A hidden or minimised window does not paint, but a screen reader still walks it, and a list realises its rows in layout. The layout is posted rather than run when a client asks, because a client's request can arrive while the window thread is in the middle of a change (a nested message pump), and a layout there rebuilt the navigation under its own rebuild.
- A live update applies an entry that sorts after the loaded entries only while more pages remain to load. Otherwise a Thread that sorted last disappeared from a fully loaded view when it changed.
- A selected group header stays selected when it expands or collapses, so Left and Right can toggle it repeatedly.
- The feed reconciles by item key and content signature. It keeps the unchanged prefix, updates a streaming message and labels in place, and rebuilds only what follows, so expanded cards, focus and the reading position survive live updates. The feed column follows its end while the user is at the end.
- Live deltas for a run that the last snapshot does not contain yet are kept until the refetched snapshot arrives. Before this, the first tokens of a new run were dropped. Run state transitions carry a sequence number, so they are appended as state events, and any gap in a run's sequence asks for a new snapshot. Feed loads are coalesced: one request is in flight, and at most one more follows it.
- Images in model output show as their alternative text with the link (client/work-inbox). Image attachments arrive with add-image-generation, which the roadmap now orders after this change.
- Screen golden images are rendered off-screen at 96 DPI from the live tree (`Window::capture`), so they do not depend on the monitor or on the window being shown.
- The composer, like the feed, stays while its Thread stays open, and it keeps the keyboard focus when the pane rebuilds for a live update. Its draft text is saved 400 ms after the last edit through a one-shot window timer, and saved at once when the composer goes. Because the field's window may already be gone by then, the composer saves its own copy of the text. The `/` skill list is a list of options driven from the field (Up, Down, Enter), as a combo box's list is.
- Removing or editing a queued message now emits the runtime's queue.changed event, as queuing does. Before this, the queued list kept a removed message until something else refreshed the feed.
- Handlers often close or rebuild what holds the control that called them: Save closes its dialog, and expanding a group rebuilds the list rows. Buttons, skill options and text-field key handlers therefore run their callbacks from a copy, and a text field checks that it still exists before it touches its state. The window also destroys its elements before its shared UI services, which the elements use. Each of these was a use-after-free that crashed only now and then.
- Each Activity rail entry is an expandable card named by its task, state and elapsed time. Its child Run's transcript is requested with `runs.transcript` the first time the entry is expanded, then kept for as long as the pane exists. A rebuild for a live update keeps expanded entries expanded and fills them again from what was already loaded. Escape closes the rail drawer.
- Search takes the Thread list's place (Ctrl+F or the list's Search button) and searches 200 ms after the last keystroke; a reply to an older query is dropped. A hit is a link named by its Thread, Archived and the snippet, with the matches in bold. Opening it opens the Thread at the message: the feed scrolls the message to its middle after the next layout pass (a one-shot timer, since timer messages come after the posted layout) and outlines it for 2.5 s. Closing the search only hides it, because the hit that closes it is still running; the query is cleared the next time it opens.
- The status bar shows the open Thread's context usage as a ring and a percentage, read from `threads.context` when the Thread opens, when a Run ends, after a compaction and after Compact now. From 75 % it warns and offers Compact now.
- Status, Settings and About are ported from the web views. Choices (language, how long verbose diagnostics stay on) are buttons that open a menu, since the toolkit has no drop-down list. A language change applies to the window at once, without the web UI's reload, so the hint now says so. The page keeps the keyboard focus on the same control when a status event rebuilds it. About no longer shows a WebView2 version.
- Approval and Question cards keep their mode (Edit arguments, Respond instead) and the text being typed until the decision is sent. After that, the next snapshot replaces the card with its resolved form. A new Action item in the open Thread is announced as the web UI's alert region was, standalone ones included. The window also passes each announcement to an observer, so the screen tests can check the announcement text without a second process.
- Removal (task 4.2): `client/ui/`, the UI packer (`uipack`) and its `.pak` reader, the UI resource, the fallback page, the WebView2 window, `WebView2Loader.dll`, the WebView2 header and the footprint spike that hosted WebView2 are gone, and the vendored-source manifest no longer lists WebView2. CI lost its Node UI job, and the release build no longer builds a UI; CI now also fuzzes the Markdown parser for two minutes. `dev.ui` reports only whether the window is open, and `app.version` has no WebView2 field. The end-to-end test now checks that opening the window starts no process at all (only worker processes may appear) and opens no listening socket.

## Risks / Trade-offs

- [Several weeks of work before parity] → Staged behind a flag (D12); the current UI keeps working until the switch.
- [A custom-drawn UI is often inaccessible] → UIA providers are part of the toolkit from the first element (D8), with automated tests and a release pass with screen readers.
- [Text input, IME and spell checking are hard to get right] → Native rich-edit controls (D3).
- [Software rendering is slower on a 4K monitor than on the measured 1600×1025 window] → Frames redraw only dirty rectangles (D2), the flip-model swap chain presents dirty rectangles, and task 1.1 on the baseline laptop includes a 4K run; the hardware device remains a measured fallback option if a budget is missed.
- [WARP device or swap chain lost, for example after a display-mode change] → Device-lost recreation from scratch, tested by simulated device removal.
- [Markdown from the model is untrusted input to a C parser] → md4c is fuzzed in the fuzz build; no HTML, no remote images, links only on click (D4).
- [A homegrown toolkit grows without bounds] → It only gets the elements the specs need; new screens reuse them.

## Open Questions

- Should the window use the Windows 11 Mica backdrop? It costs little, but it is purely cosmetic. Decide after the shell spike.
