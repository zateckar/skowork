# Proposal

## Why

Many of our business applications have no API, MCP server or web front end that the agent could use: desktop line-of-business clients, legacy Win32 tools and Office dialogs. Computer use lets the agent operate these applications the way a user does. It combines the Windows accessibility tree with screenshots that the on-prem vision model reads. It must stay visibly under the user's control and within policy at all times.

## What Changes

- Add the `computer.*` tools:
  - **Perception:** the UI Automation tree is the primary source. DPI-aware, downscaled screenshots go to the vision model role.
  - **Actions:** UI Automation patterns are used first, with synthesized input (click, type, keys, scroll, drag) as the fallback.
- Add safety controls:
  - a visible "agent is controlling" overlay;
  - a global stop hotkey that cancels the Run within 1 second;
  - agent control stops when the user touches the mouse or keyboard, and the Run waits on a Continue/Stop Question;
  - per-application allow and deny lists;
  - no typing into password fields;
  - a risk class on every action, which the client assigns itself, with decisions made by client/policy-enforcement.
- Define the limits: elevated windows, the secure desktop (UAC, Ctrl+Alt+Del) and the lock screen cannot be driven. The agent reports each case as a tool error and asks the user, through a client-raised Question, to complete the step.
- Screenshots stay in memory only for the duration of the Run unless policy allows retention. In Unattended runs, every action other than observe and screenshot needs approval unless policy.unattended.rules explicitly allows it.
- Add an accessibility-tree-only mode for when the vision model's grounding is not good enough.

Out of scope:
- Driving other users' sessions, remote desktops or virtual machines.
- Browser pages; those use client/browser-use.
- Recording macros for replay.

## Capabilities

### New Capabilities
- `client/computer-use`: perception, actions, user control, application scoping, and limits for operating Windows desktop applications

### Modified Capabilities
- None.

## Impact

- **Roadmap:** change 8 of 13 (Managed).
- **Depends on:**
  - add-client-foundation
  - add-agent-runtime: tool calls, the vision model role and image input rules, client-raised Questions (Continue/Stop), cancellation, and provenance tagging of on-screen content
  - add-policy-and-sandbox: risk-class decisions, the tool-declared decision floor, policy.tools.rules and policy.unattended.rules, and the untrusted-content rule that covers on-screen content
  - add-work-inbox: presentation of Approval and Question Action items
- **No dependency on add-scheduled-and-remote-tasks (change 10).** In an Unattended run, an approve decision simply remains an Approval Action item under client/policy-enforcement and client/agent-runtime.
- **New code:** the `client/` computer-use module and the overlay window.
- **Policy keys owned by client/computer-use:**
  - `policy.tools.computer.mode` (off | allow, default off)
  - `policy.tools.computer.perception` (tree_and_vision | tree_only, default tree_and_vision)
  - `policy.tools.computer.app_allowlist` (default empty)
  - `policy.tools.computer.app_denylist` (default empty)
  - `policy.tools.computer.stop_hotkey` (default Ctrl+Alt+End)
  - `policy.tools.computer.retain_screenshots` (default false)
- **Policy keys used here but owned by client/policy-enforcement:** `policy.tools.rules`, `policy.tools.risk` and `policy.unattended.rules` entries for `computer.*`.
- **Model dependency:** a vision model must be mapped to the vision role in control-plane/llm-gateway.
