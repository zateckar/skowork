# Proposal

## Why

On 2026-11-02 the product owner asked for two changes:
- The window should look like a Windows 11 app. Apps such as File Explorer draw their own title bar, which carries the app's content and blends with it. Workbench uses the plain system title bar instead, so it sits apart from the app's colours.
- Workbench has no icon of its own. The executable and the taskbar show the generic Windows application icon, and the tray shows a plain coloured disc.

## What Changes

- **Title bar.** The window has no separate title bar row. As in File Explorer, where the tabs sit in the title bar, the panes reach the top edge and their top row is the title bar: the navigation pane starts with the Workbench mark and name, the pane headings (Inbox, the Thread's title and toolbar) sit beside it, and the pane at the top-right corner ends with Minimize, Maximize (Restore down when maximised) and Close. Everything else Windows does with a title bar still works on the empty parts of that row:
  - dragging, and double-clicking to maximise or restore;
  - the window menu (right-click, Alt+Space);
  - resizing at every edge;
  - the Snap layouts that appear when the pointer rests on Maximize;
  - the rounded corners and shadow of Windows 11.
  The buttons have accessible names and tooltips. A high-contrast theme uses the system colours.
  (Revised on 2026-11-02 after review: the first version drew a full-width title bar, which the product owner rejected because File Explorer has none.)
- **App icon.** A Workbench icon (the Emerald square with an Electric Green "S"):
  - is built into Workbench.exe, so File Explorer, Start and shortcuts show it;
  - is the window's icon, so the taskbar and Alt+Tab show it;
  - is the tray icon. The tray state (disconnected, idle, running, action required, paused) shows as a coloured dot on it, and unread Chat messages as the red badge, as before.

Out of scope:
- The Mica backdrop. The window draws its own opaque surfaces.
- Tabs in the title bar.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/desktop-host`: Window Title Bar (added) and App Icon (added).

## Impact

- **Roadmap:** 25, after improve-desktop-look. Depends on add-client-foundation (Tray Presence) and add-native-ui (the native window).
- **Code:**
  - client/src/ui: the window frame (non-client area, hit testing, the window buttons) and the space headers keep for the buttons;
  - client/src/shell: the mark and name at the top of the navigation pane, and pane headers in the top row;
  - client/src/app: the icon resource;
  - client/src/host: the tray icon drawn from the app icon;
  - strings in en, de, cs.
- **Tests:** hit testing of the title bar, the caption buttons' names and actions, the tray icon states, and updated golden images.
- **Data:** none.
