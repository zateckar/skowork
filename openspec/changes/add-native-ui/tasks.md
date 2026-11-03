## 1. Validation spikes

- [ ] 1.1 Prototype the Direct2D/DirectWrite shell with a virtual Thread list of 10,000 synthetic Threads and a feed of 200 Markdown messages. Verify first content ≤ 300 ms, 60 fps scrolling with software rendering (also on a 4K monitor), and the core working set ≤ 150 MB on the baseline laptop; record the numbers in design.md
- [ ] 1.2 Prototype UI Automation fragment providers for a list, a button and an expandable card. Verify with Narrator and NVDA that names, roles and a notification event are announced
- [ ] 1.3 Prototype the composer as a rich-edit control with IME and spell checking inside the Direct2D window. Verify Czech diacritics, a Japanese IME, paste and undo

## 2. Toolkit

- [x] 2.1 Implement the software (WARP) device and swap-chain layer with dirty-rectangle presents, device-lost recovery, per-monitor DPI and theme changes. Verify with tests that render off-screen, simulate device removal and change DPI
- [x] 2.2 Implement the element tree, layout, invalidation, hit testing and focus chain. Verify with unit tests for layout and keyboard focus order
- [x] 2.3 Implement the elements of design D2, including the virtual list and text-layout caches. Verify with unit and golden-image tests
- [x] 2.4 Vendor md4c (pinned, with licence in the manifest) and implement Markdown rendering per client/work-inbox Markdown Rendering. Verify with tests for each block type, streaming re-parse, raw HTML as text, no fetch of image URLs, and a fuzz target for the Markdown path
- [x] 2.5 Implement the UIA providers and the notification event for every element. Verify with UIA-driven tests that every control has a role and name and is reachable by keyboard
- [x] 2.6 Implement the string resources, the build-time key check and the plural rules for cs, de and en. Verify that a missing key fails the build and with plural tests for Czech

## 3. Screens (behind the development flag)

- [x] 3.1 Shell: window, three panes, Activity rail drawer below 1180 px. Verify with UIA tests at two widths
- [x] 3.2 Thread list, smart views, folders, pins, date groups and triage actions. Verify against the client/work-inbox scenarios with UIA tests
- [x] 3.3 Thread feed: streaming Markdown, tool-call cards, reasoning blocks, inline images, error banners with remedy. Verify with UIA and golden-image tests. Images in model output show as their alternative text with the link; image attachments come with add-image-generation 3.3, now ordered after this change
- [x] 3.4 Composer: attachments, drafts, `/` skills, queue or steer, Stop, fork. Verify with UIA tests
- [x] 3.5 Approval and Question cards, and standalone Action items. Verify with UIA tests including the screen-reader announcement
- [x] 3.6 Activity rail with lazily loaded child Run transcripts. Verify with UIA tests
- [x] 3.7 Search with jump-to-message, keyboard shortcuts, status bar and settings structure. Verify with UIA tests

## 4. Switch and removal

- [ ] 4.1 Measure the client/desktop-host Window Footprint budgets on the baseline laptop. Verify all four budgets and record them in design.md
- [x] 4.2 Make the native window the only UI. Remove `client/ui/`, the UI packer and resource, `WebView2Loader.dll` and the WebView2 header; update the vendored-source manifest. Verify a clean build and that opening the window starts no process
- [x] 4.3 Update add-mail-calendar: interactive Microsoft sign-in through the system browser with a loopback redirect instead of a WebView2 window (spec, design and tasks). Verify with `openspec validate --all --strict`
- [ ] 4.4 Manual accessibility pass with NVDA and Narrator, and a high-contrast check. Verify that every function works by keyboard alone and that Action items are announced
- [x] 4.5 Run `openspec validate --all --strict`
