# Design

## Context

The native window (add-native-ui) is a standard WS_OVERLAPPEDWINDOW with the system title bar. apply_title_bar only switches the caption to dark mode. The executable has no icon resource, so Windows shows the generic one. The tray draws a coloured disc per state (add-client-foundation Tray Presence).

## Goals / Non-Goals

- **Goals:**
  - a title bar that blends with the app and keeps every system behaviour of a title bar: move, double-click, the window menu, resize, Snap layouts, rounded corners and shadow;
  - one brand icon for the executable, the window and the tray.
- **Non-Goals:**
  - Mica;
  - tabs or other content in the title bar.

## Decisions

### D1. Custom frame
- **Choice:**
  - The window keeps WS_OVERLAPPEDWINDOW, so Windows still gives it the shadow, the rounded corners, the window menu, snapping and the minimise and maximise animations. WM_NCCALCSIZE lets Windows compute its frame, then gives the top back to the client area: the system keeps its resize frame on the left, right and bottom, and the caption becomes client area. While maximised, the frame that lies beyond the monitor at the top is taken back (SM_CYFRAME plus SM_CXPADDEDBORDER).
  - DwmExtendFrameIntoClientArea with a one-pixel top margin keeps the system border and shadow.
  - WM_NCACTIVATE is passed on with lParam -1, so Windows does not paint the old caption.
- **Why:** this is how Windows 11 apps with their own title bar keep the system behaviours. Drawing everything ourselves would lose Snap layouts and the window animations.
- **Alternatives considered:**
  - Colouring the system caption (DWMWA_CAPTION_COLOR). It is only a colour change, and the title bar still sits apart from the content.
  - DwmDefWindowProc with system-drawn caption buttons. Our renderer paints the whole client area opaque, so the system buttons would be covered.

### D2. Hit testing
- **Choice:** Windows answers WM_NCHITTEST for the frame it keeps: the left, right and bottom edges and the corners. Everything else is answered from the window's layout, in this order:
  1. The top resize edge, when the window is not maximised. Its height is the system frame, SM_CYFRAME plus SM_CXPADDEDBORDER at the window's DPI.
  2. The caption buttons, as HTMINBUTTON, HTMAXBUTTON and HTCLOSE. HTMAXBUTTON is what makes Windows 11 offer Snap layouts.
  3. In the caption band (the top 40 DIPs), a point over a control, as HTCLIENT. A control is the deepest node at the point or one of its ancestors that is enabled and focusable, invocable, a native text field, or has its own mouse cursor (a splitter).
  4. The rest of the caption band, as HTCAPTION: pane backgrounds, headings, the mark and name. Windows then handles dragging, double-clicking and the window menu.
  5. Everything else, as HTCLIENT.

  The pure function `frame_hit(FrameLayout, x, y)` decides this, so it can be unit-tested. FrameLayout holds the caption band height, the top border, whether the window is maximised and the button frames, all in DIPs; whether the point is over a control comes from the element tree's hit test.

  Mouse messages over the caption buttons arrive as non-client messages (WM_NCMOUSEMOVE, WM_NCLBUTTONDOWN, WM_NCLBUTTONUP, WM_NCMOUSELEAVE). The window maps them onto the buttons' hover and pressed states and their tooltips; a release over the button that was pressed invokes it. Right clicks on the buttons are swallowed, so the window menu opens only from the caption. Windows never draws its own buttons.
- **Why:** returning the system codes keeps Windows features that depend on them.

### D3. No title bar row: the panes' top row
- **Choice:**
  - The content fills the window from the top edge; there is no TitleBar row. (The first version had a 40-DIP full-width title bar. On review the product owner pointed at File Explorer, which has none: its tabs sit in the title bar.)
  - WindowButtons, a toolkit node, holds three CaptionButton elements, 46 by 40 DIPs, with the Segoe Fluent Icons glyphs Minimize E921, Maximize E922, Restore E923 and Close E8BB. Close turns red (#C42B1C with white) on hover. The glyphs are dimmed while the window is inactive. It floats at the window's top-right corner above the content and the modal layer, so a dialog never covers it.
  - CaptionSpace, a toolkit node, ends a pane's header row. It takes the width of the window buttons only while its row reaches the window's right edge within the caption band, so whichever pane is at the top-right (the Thread, the Activity rail, a page, a Chat, or the list on a narrow window) keeps its toolbar clear of the buttons. It decides after layout and asks for one more layout pass when that changes; the row's right edge does not depend on its width, so this settles at once.
  - The navigation pane starts with the Workbench mark (an Emerald rounded square with an Electric Green "S", BrandMark) and the name, 40 DIPs high; on a compact navigation pane only the mark. The pane headings sit in the same band.
  - The caption buttons are not in the Tab order, as in other Windows apps: the keyboard reaches what they do through the window menu (Alt+Space) and Win+Arrow keys. Node::system_keyboard tells the accessibility check so. UI Automation exposes them as buttons with the Invoke pattern. Invoking one posts WM_SYSCOMMAND (SC_MINIMIZE, SC_MAXIMIZE or SC_RESTORE, SC_CLOSE). Their names come from the strings, and Maximize is renamed "Restore down" while maximised.
  - The modal layer dims the whole content. The caption band still answers HTCAPTION outside the dialog, so the window can be moved while a dialog is open.
  - Window::Options.custom_frame turns this on. It is on by default, and the hidden-window tests keep it.
- **Why:** the panes' own headings make the title row, as in File Explorer; no space is spent on a bar that only repeats the app's name. The window buttons and the space for them are toolkit elements, so every pane layout gets them without knowing which pane is at the corner.
- **Alternatives considered:** a full-width title bar in the navigation pane's colour (the first version, rejected on review); giving each pane a fixed right inset, which wastes the space in panes that are not at the corner.

### D4. Icon
- **Choice:**
  - client/src/app/workbench.ico holds PNG images at 16, 20, 24, 32, 40, 48, 64 and 256 pixels: an Emerald (#0E3A2F) rounded square with an Electric Green (#78FAAE) bold "S".
  - client/tools/make-icon.ps1 makes the file with System.Drawing, so the icon can be made again.
  - The resource script carries it as ICON 1. The window class loads it, and the window sets its large and small icons for its DPI (again after a DPI change).
  - The tray loads the icon at the small-icon size and draws on it:
    - the state dot, in the bottom-right corner with a dark ring (#131A17) that keeps it apart from the mark, in the existing state colours: grey disconnected, green idle, blue running, orange action required, amber paused. Paused shows a hollow dot.
    - the red unread badge, in the top-right corner as before.
  - Without the resource (tests), the tray falls back to the disc of before.
- **Why:** one mark everywhere. The state stays readable at a glance, as Tray Presence asks.

## Risks / Trade-offs

- [Custom frames break on edge cases such as an auto-hidden taskbar or a maximised window on another monitor] → The maximised inset follows the system frame metrics. Auto-hide is accepted as-is: the window covers the screen and the taskbar shows on hover.
- [Golden images change] → They are regenerated once and reviewed by eye.

## Open Questions

- None.

## Acceptance (2026-11-02, demo client, dark theme, 4K monitor, taskbar on the right)

- The window shows the Workbench title bar: the mark, the title, and Minimize, Maximize and Close on the navigation pane's colour. Windows paints no caption of its own.
- Invoking title-bar-maximize through UI Automation maximised the window. The button was then named "Restore down" and showed the Restore glyph, and the title bar stayed fully on screen. Invoking it again restored the window and the name went back to "Maximize".
- Workbench.exe carries the icon (ExtractAssociatedIcon gives the mark), and its own manifest is kept. The taskbar button shows the mark. The tray shows the mark with the green idle dot.
- Not checked: Snap layouts on hovering Maximize. This needs a by-hand hover. The button answers HTMAXBUTTON, which is what Windows offers Snap layouts on (checked by UiWindow.FrameHitTesting and UiWindow.OwnTitleBarWithNamedButtons, since replaced by UiWindow.WindowButtonsAndCaptionBand).

## Acceptance of the revision (2026-11-02, demo client, dark theme)

- No title bar row. The navigation pane, the list and the reading pane reach the top edge, and their top rows form the caption band. The navigation pane starts with the mark and "Workbench". Minimize, Maximize and Close sit in the top-right corner over the rightmost pane, and that pane's header keeps clear of them (CaptionSpace). This matches the File Explorer layout of the example.
- Dialogs dim the content but not the window buttons (New Chat dialog screenshot).
- Invoking title-bar-maximize through UI Automation maximised the window to the work area. The button was renamed "Restore down" and stayed at the screen's top-right. Invoking it again restored the window and the name "Maximize".
- Tests: UiWindow.FrameHitTesting, UiWindow.WindowButtonsAndCaptionBand (a heading in the band is HTCAPTION, New thread is HTCLIENT, the buttons answer HTMINBUTTON, HTMAXBUTTON and HTCLOSE) and UiWindow.CaptionSpaceKeepsTheCornerClear. Golden images regenerated and reviewed.
- Still not checked: Snap layouts on hovering Maximize (by hand, with the user's consent).