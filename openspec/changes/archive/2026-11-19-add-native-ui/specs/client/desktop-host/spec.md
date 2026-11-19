# Spec Delta

## MODIFIED Requirements

### Requirement: On-Demand UI Window
The client SHALL host its user interface in a native window that the client process draws itself, created only when the user opens it. No web engine or other process may be started to show the user interface. The window's rendering resources (graphics device, swap chain, text layouts and image caches) MUST be released when the user closes it, while the client keeps running in the tray.

#### Scenario: Close window keeps agent running
- **WHEN** the user closes the main window while a Run is active
- **THEN** the Run continues in the background
- **AND** the window's rendering resources are released

#### Scenario: No helper processes for the window
- **WHEN** the user opens the main window
- **THEN** no new process is started to show it

## ADDED Requirements

### Requirement: Window Footprint
On the baseline hardware, with the main window open on a Thread of 200 messages, the client SHALL stay within these budgets:
- the core process working set is 150 MB or less;
- the window shows its first content 300 ms or less after the user opens it;
- scrolling the Thread list or the feed keeps 60 frames per second;
- a typed character appears in the composer within 16 ms.

#### Scenario: Window open budget
- **WHEN** the window has been open for one minute on a Thread of 200 messages
- **THEN** the core process working set is at most 150 MB, and no other process belongs to the window
