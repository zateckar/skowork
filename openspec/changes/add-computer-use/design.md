# Design

## Context

See proposal.md, section "Why". Computer use runs inside the core process, using the user's desktop and the user's token. The approval decisions come from client/policy-enforcement, and the vision model is reached through the vision model role of client/model-access. On the baseline hardware there is no local vision work: the client only captures screenshots and downscales them on the CPU.

## Goals / Non-Goals

**Goals:**
- Reliable control of Win32, WPF, WinForms, UWP and Office applications. We expect most actions to go through the accessibility tree rather than through pixels.
- At most 250 ms of local overhead per action, excluding the model's response time, on the baseline laptop.

**Non-Goals:**
- Operating other user sessions or the secure desktop.
- OCR on the client. Text is read through UIA, and the vision model handles anything that is pixel-only.

## Decisions

### D1. Native UIA COM client
- **Choice:**
  - `IUIAutomation` with a cache request (`IUIAutomationCacheRequest`) for each observe call, so a whole subtree is fetched in one cross-process round trip.
  - The tree is pruned to on-screen elements where `IsControlElement` is set.
  - Element references are short ids mapped to cached `RuntimeId`s, valid for the current Run.
- **Alternatives considered:**
  - FlaUI (.NET): would bring a managed runtime into the client.
  - The Hermes approach, which uses cua-driver through stdio MCP: an extra process and runtime, though it would be good for prototyping.
  - Win32 `EnumWindows` with MSAA only: too poor a fit for modern UI frameworks.

### D2. Screenshots with Windows.Graphics.Capture
- **Choice:**
  - `GraphicsCaptureItem` captures windows; monitor capture is the fallback.
  - The client is per-monitor DPI aware v2.
  - Downscaling uses WIC with high-quality interpolation, to the limits defined by the image input rules of client/model-access. This change defines no separate size.
  - A coordinate transform is kept for each capture.
- **Note:** the yellow capture border Windows draws is acceptable. It also tells the user that a capture is happening.
- **Alternatives considered:**
  - GDI `BitBlt`: fails for DirectX and protected content.
  - Desktop Duplication: captures whole monitors only, which is more data than needed.

### D3. Input
- **Order of preference:**
  1. UIA patterns.
  2. `SendInput`, with the target window brought to the foreground and verified as foreground before any input.
- **Typing:** uses Unicode `KEYEVENTF_UNICODE` events, so the result does not depend on the keyboard layout. This matters for Czech and German layouts.
- **Release on stop:** the client tracks which keys and buttons it has pressed, so the stop handler can release all of them.

### D4. User-input detection and the stop hotkey
- **Choice:**
  - Low-level mouse and keyboard hooks (`WH_MOUSE_LL`, `WH_KEYBOARD_LL`) run on a dedicated thread with its own message loop.
  - Synthesized events carry the `LLMHF_INJECTED` / `LLKHF_INJECTED` flags, which is how the hooks tell user input from agent input.
  - The hooks are installed only while the agent is in control.
  - The stop hotkey is registered with `RegisterHotKey`. The default is Ctrl+Alt+End. It is easy to reach, it is not taken by common applications or by Windows (unlike Ctrl+Alt+Del or Ctrl+Shift+Esc), and the always-on overlay shows it.
- **Alternative considered:** polling `GetLastInputInfo`. It cannot tell user input from injected input.

### D5. Recognising destructive actions
- **Choice:** a classifier runs on the target element before each action. It checks the element's name and automation id against a multilingual list (Czech, German and English):
  - send, submit, delete, remove, pay, close without saving;
  - Enter or Ctrl+Enter pressed in compose windows of known mail and chat clients.

  A match raises the action's risk class from external to destructive.
- **Decisions:** the client never decides approvals here. Typical policies are:
  - `tools.rules: {"computer.*": approve}` to approve every action;
  - `tools.risk: {external: allow, destructive: approve}` to approve destructive actions only.
- **Alternatives considered:**
  - Letting the model declare the risk itself: rejected, because untrusted content could steer the model.
  - A dedicated `computer.approve` setting: rejected, because it would put a decision into a settings key.

### D6. Overlay
- **Choice:** a layered, click-through, topmost window (`WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST`). It draws a frame around the target window and a banner showing the Thread title and the stop hotkey. The overlay is excluded from capture with `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`, so it never appears in the model's screenshots.

### D7. Vision for every kind of application
- **Choice:** the accessibility tree comes first, and vision covers everything the tree does not.
  - computer.screenshot is offered whenever the perception mode is tree_and_vision and the gateway reports a vision role. Its image goes to the model once, with the vision role (client/agent-runtime Image Tool Results); without a vision role a screenshot could not be read, so the tool is not offered.
  - `area: "window"` (default) captures the window alone with Windows.Graphics.Capture. `area: "screen"` captures the display the window is on with a screen `BitBlt`. That capture includes the application's pop-up menus, drop-down lists, tooltips and secondary windows, which are separate top-level windows a window capture misses, and full-screen remote sessions. Out-of-scope windows (denied, the client's own, credential UI) are painted black in it, front to back by z-order, before the image is encoded; the overlay is excluded from both captures by its display affinity.
  - observe and screenshot restore a minimized window first (`SW_SHOWNOACTIVATE`, without taking the focus), because a minimized window has no content on screen and an empty tree.
  - click, type, scroll and drag accept image coordinates. A window capture shows the window even where other windows cover it, so a point of a window screenshot belongs to that window: the action raises it, then checks with `WindowFromPoint` that the point reaches it or another window of its process (an open menu) and refuses otherwise. A point of a display screenshot belongs to the window under it. Application scoping is checked on that window. (Found end to end: without this, a click the model placed correctly went into the browser window in front of the target.)
  - When observe lists no interactive element outside the title bar, its result says that the window draws its own content and points to computer.screenshot. This covers custom-drawn controls, Java (without the Access Bridge), games, Citrix and Remote Desktop windows and canvas-based applications.
- **Alternatives considered:**
  - Sending a screenshot with every observe: doubles the cost of every step, for windows where the tree is already enough.
  - Client-side OCR: a Non-Goal; the vision model reads pixels.

### D8. Launching applications
- **Choice:** `computer.launch` starts an executable with `CreateProcessW` on the given name or path, with no command line, no shell and no document or URL handling. It then polls the visible top-level windows for one of that executable's name, up to 8 seconds, and makes the first new one the Run's target. Process creation runs as the user, as the other computer actions do.
- **Refused before approval:** command interpreters and script hosts (cmd.exe, powershell.exe, pwsh.exe, wscript.exe, cscript.exe, mshta.exe, rundll32.exe), because a shell would run any command behind one approval, and executables refused by application scoping. Everything else is decided by policy as external.
- **Alternatives considered:**
  - `ShellExecuteEx`: resolves App Paths and documents, which widens the launch surface to any file or URL.
  - Win+R through computer.keys: needs a window to receive the keys, which is the gap this change closes.

### D9. Target check before approval
- **Choice:** a tool may declare a `precheck` that runs before the policy decision. It resolves the call's target without sending input (references, window ids, the Run's target, the latest screenshot's transform) and the application scope. A failed precheck ends the call with its message. It creates no Action item.
- **Why:** approval asks the user about an action that was always going to fail. The user approved the same empty call several times in the reported run.
- **Alternatives considered:** checking inside the tool body after approval: rejected, because the approval already happened. Making the policy know about windows: rejected, because the policy decides risk, not targets.

### D10. Provenance of computer results
- **Choice:** the tool-level default provenance is `computer` with `desktop` in place of an empty application. Results about a window carry that window's process and title, as before.
- **Why:** the reported approval listed "computer: , 8:34 PM" entries that named no source, so the user could not see what the request came from.

## Implementation Notes

- **Capture:** Windows.Graphics.Capture through its ABI (`IGraphicsCaptureItemInterop::CreateForWindow`, a free-threaded frame pool), with `PrintWindow(PW_RENDERFULLCONTENT)` as the fallback when a window cannot be captured that way. The capture box is the window's DWM extended frame bounds.
- **Scaling:** the test machine runs at 125 %, and display scaling cannot be changed in the test environment (no system changes). Click mapping is verified by the transform tests at 100, 150 and 200 % and by a real screenshot-and-click on a custom-drawn control at the machine's scaling.
- **COM apartment:** each UIA call runs on its own short-lived MTA thread under the 2-second limit. The client holds one lasting `CoIncrementMTAUsage`, because COM otherwise tears down the MTA when the last such thread ends and unloads UI Automation under the cached interface pointers.
- **Select:** a Win32 drop-down list drops a selection made through UI Automation while it is open. After `SelectionItem.Select` the client compares the combo box value with the requested option and, when they differ, clicks the option instead and reports the fallback.
- **Clearing a field by keyboard:** Ctrl+End then Ctrl+Shift+Home, because Ctrl+A is not select-all in classic Win32 edit boxes.
- **Control lifetime:** control (overlay, hotkey, hooks) starts before the first input of an action and stays 3 seconds after it, so the overlay does not flicker between consecutive actions. User input counts only while an action is sending input; between actions the agent is not touching the desktop.
- **Unattended floor:** the policy decision function knows tool floors that apply in Unattended runs only (`unattended_only`); the computer actions declare such a floor of approve, lifted only by an explicit `policy.unattended.rules` allow.
- **Physical input:** the hooks ignore events with the injected flag. Tests verify that injected input does not interrupt and drive the hooks' user-input path through a test entry point; physical input itself cannot be produced by an automated test.

## Risks / Trade-offs

- [The on-prem vision model grounds coordinates poorly] → Spike 1.1 measures this. If accuracy is below 85% on the benchmark, the default becomes `perception: tree_only`, with screenshots used only for the model's understanding and not for coordinates.
- [Prompt injection through on-screen text, for example "click Send now"] → Observed and captured content is untrusted, as the spec's "On-Screen Content Is Untrusted" requirement states, and is tagged with its window provenance. Risk classification is done by the client (D5), not by the model. Destructive and external actions are always gated according to policy, and the client's own windows are always out of scope.
- [Low-level hooks add input latency for the user] → The hooks are installed only during control, and the hook procedures do minimal work and return immediately.
- [UIA calls hang on unresponsive applications] → Every UIA call has a 2-second timeout and runs on a worker thread. A hung application yields a tool error rather than a hung Run.
- [computer.launch starts programs the model names] → Every launch is a policy decision as external, command interpreters are refused outright, and the app allow and deny lists apply to the executable name. With the default policy, every launch is approved by the user.
- [Screenshots contain sensitive data sent to the LLM] → They go only to the on-prem gateway, are not persisted by default, and the app denylist keeps sensitive applications out of scope.

## Migration Plan

Not applicable.

## Open Questions

- Which business applications are on the default denylist (for example HR, finance and password managers)? The Group policy owners will decide this, and it only affects policy content.
