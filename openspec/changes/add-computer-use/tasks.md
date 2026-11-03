# Tasks

## 1. Validation spikes

- [ ] 1.1 Build a grounding benchmark of 100 tasks across 10 organization Windows applications (Office, SAP GUI, a legacy Win32 client, a WPF tool). Measure the on-prem vision model's click accuracy from screenshots against tree-only. Verify that the results and the chosen default perception mode are recorded in design.md
- [ ] 1.2 Measure UIA subtree observe latency with a cache request on the baseline laptop for Outlook, Excel and SAP GUI. Verify p95 is under 250 ms or document the mitigation

## 2. Perception

- [x] 2.1 Implement computer.observe (cached UIA subtree, pruning, element references). Verify with tests against a WinForms/WPF/UWP test application
- [x] 2.2 Implement computer.screenshot (Windows.Graphics.Capture, DPI awareness, downscale, coordinate transform). Verify that click mapping is correct at 100%, 150% and 200% scaling
- [x] 2.3 Implement tree-only mode. Verify that the screenshot tool is absent and coordinate actions are rejected
- [x] 2.5 Implement vision for every kind of application (design D7): computer.screenshot only while the vision role is available, the display capture with out-of-scope windows blacked out, and the observe hint for windows without interactive elements. Verify against Win32, WinForms, WPF and Chromium windows, and verify end to end that the real model operates a custom-drawn application through screenshots alone
- [x] 2.4 Tag observe and screenshot results as untrusted with application and window provenance through client/agent-runtime. Verify with a test window containing injected instructions that the content arrives delimited as untrusted and no decision changes

## 3. Actions

- [x] 3.1 Implement click, type, keys, scroll, drag and select with the UIA-pattern-first strategy and SendInput fallback. Verify with the test application and fallback reporting
- [x] 3.2 Implement the destructive-action classifier (Czech, German and English) and risk-class assignment. Verify with unit tests over a labelled element set
- [x] 3.3 Implement the password-field and credential-prompt refusal. Verify against standard password boxes and the Windows credential UI

## 4. Safety controls

- [x] 4.1 Implement the overlay with capture exclusion. Verify the overlay is visible on screen and absent from captured screenshots
- [x] 4.2 Implement the global stop hotkey and release of held input. Verify in an automated test that the Run is cancelled within 1 s during a drag
- [ ] 4.3 Implement user-input detection that stops control and raises the Continue/Stop Question through client/agent-runtime. Verify that physical input interrupts the Run and injected input does not
- [x] 4.4 Implement app allow/deny scoping, always-deny for self and credential UI, and unreachable-surface errors (elevated, secure desktop, lock screen) followed by a client-raised Question. Verify with tests for each case
- [x] 4.5 Implement in-memory screenshot handling, retention under policy, and the Unattended decision floor of approve. Verify that no screenshot files exist after a default Run and that an Unattended click yields an Approval Action item

## 5. Integration

- [ ] 5.1 End-to-end: from a Thread, fill and save a form in a test line-of-business application with policy.tools.risk set to external allow and destructive approve. Verify approvals, overlay, stop and telemetry tool outcomes
- [x] 5.2 Run `openspec validate --all --strict` and fix any findings

## 6. Launching, target checks and provenance

- [x] 6.1 Implement computer.launch (design D8): CreateProcessW on an executable name or path, refusal of command interpreters and scoped-out executables before approval, and the 8-second wait for the new window that becomes the Run's target. Verify with unit tests of the refusal and argument rules; verify once in the desktop test that Notepad opens and its window id works for computer.type
- [ ] 6.2 Implement the precheck hook that runs before the policy decision (design D9) and use it for every computer.* tool. Verify with a unit test that a call with no target creates no approval and that a call with a valid target still reaches policy
- [x] 6.3 Tag failure results with the `desktop` provenance instead of an empty source (design D10). Verify with a unit test that no computer result has an empty application
- [x] 6.4 Run `openspec validate --all --strict` after the changes above

## Workflow follow-up

- Archive with `openspec archive add-computer-use --yes` after review.
