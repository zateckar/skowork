# Spec Delta

## ADDED Requirements

### Requirement: Window Title Bar
The main window SHALL have no separate title bar row, as in Windows 11 File Explorer: the panes reach the top edge and their top row is the title bar. The navigation pane starts with the Skowork mark and name, and the top-right corner holds Minimize, Maximize (Restore down while maximised) and Close with accessible names and tooltips. Empty parts of the top row move the window, maximise on double-click and open the window menu. Resizing and Snap layouts work as usual.

#### Scenario: Window buttons
- **WHEN** a screen reader user moves through the window
- **THEN** it finds the buttons "Minimize", "Maximize" and "Close", and invoking Maximize maximises the window and renames the button "Restore down"

#### Scenario: Snap layouts
- **WHEN** the pointer rests on Maximize on Windows 11
- **THEN** Windows shows the Snap layouts for the window

#### Scenario: Moving and resizing
- **WHEN** the user drags an empty part of the top row, such as beside the Inbox heading, or an edge of the window that is not maximised
- **THEN** the window moves, or resizes from that edge

#### Scenario: Title bar clicks
- **WHEN** the user double-clicks an empty part of the top row, or right-clicks it, or presses Alt+Space
- **THEN** the window maximises or restores, or the window menu opens

#### Scenario: Controls in the top row
- **WHEN** the user clicks the Search button or a Thread's toolbar in the top row
- **THEN** the control acts as usual and the window does not move

### Requirement: App Icon
Skowork SHALL have its own icon: an Emerald square with an Electric Green "S". It is built into Skowork.exe, it is the main window's icon in the taskbar and in Alt+Tab, and it is the tray icon. On the tray icon, a coloured dot shows the tray state (disconnected, idle, running, action required, paused), and a red badge shows unread Chat messages.

#### Scenario: Executable and taskbar
- **WHEN** the user looks at Skowork.exe in File Explorer, or at the taskbar while the window is open
- **THEN** both show the Skowork icon

#### Scenario: Tray state
- **WHEN** a Thread gets an Action item
- **THEN** the tray shows the Skowork icon with the action-required dot
