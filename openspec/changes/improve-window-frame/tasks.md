## 1. Icon

- [x] 1.1 The icon file and its script, the ICON resource, and the window class icons (D4). Verify: Skowork.exe carries the icon (ExtractIconEx finds one), and the window's large and small icons are set
- [x] 1.2 The tray icon from the app icon with the state dot and the unread badge (D4). Verify with host tests: each state makes an icon, and the fallback works without the resource

## 2. Title bar

- [x] 2.1 The custom frame (D1) and the hit testing (D2). Verify with UI tests of frame_hit: edges, buttons, caption and client, maximised and not
- [x] 2.2 The TitleBar and CaptionButton elements, the modal layer leaving the title bar undimmed, strings in en, de, cs (D3). Verify with hidden-window UI tests: the buttons' names and roles, the window title shown, the content under the title bar, hit testing through the window, no accessibility problems; golden images regenerated. Maximize renamed after maximising is checked on the demo client (3.1), since maximising would show the hidden window

- [x] 2.3 Revision after review (D3): no title bar row. WindowButtons at the top-right corner, CaptionSpace in the pane headers, the mark and name at the top of the navigation pane, hit testing of the caption band through the element tree (D2). Verify with UI tests: a heading in the band is HTCAPTION, a toolbar button in it HTCLIENT, the window buttons' codes, the rightmost header keeps clear of the buttons; golden images regenerated

## 3. Acceptance

- [ ] 3.1 On the demo client: screenshot of the title bar in the dark theme; maximise and restore; Snap layouts on Maximize (by hand, with the user's consent); the icon in the taskbar, in the tray and on Skowork.exe. Record the result in design.md
- [x] 3.2 Run `openspec validate --all --strict`
