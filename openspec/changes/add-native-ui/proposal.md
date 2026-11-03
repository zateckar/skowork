# Proposal

## Why

The client's purpose is to be lean and fast on the baseline laptop (i5, 16 GB, integrated GPU). add-client-foundation decision D4 put the user interface in a WebView2 single-page app (Svelte and TypeScript). That brings a browser engine with it: a browser process, a GPU process and renderer processes, each with its own memory, plus a second technology stack (TypeScript, a JavaScript build, a mock host) beside the C++ client. The product owner reversed that decision on 2026-10-23: the client is a C++ Win32 application, and its user interface is native Win32 with Direct2D and DirectWrite.

Testing the current UI also showed a gap: the Thread feed shows the model's Markdown as source text, and no requirement says how model output is formatted.

## What Changes

- **BREAKING (implementation):** replace the WebView2 window and the `client/ui` single-page app with a native window, drawn by the client process with Direct2D and DirectWrite. It uses native rich-edit controls for text input and UI Automation for accessibility. No web engine runs for the user interface.
- The behaviour in client/work-inbox stays as specified: list, views, cards, composer, search, shortcuts, status bar, settings, localization and accessibility. Only the way it is built changes.
- Add Markdown rendering of model output in the Thread feed. Links in model output are untrusted, and remote images in Markdown are never fetched.
- Add a footprint budget for the open window, so the lean goal is measured and not only stated.
- Update the project's technology decisions (`openspec/config.yaml`) and mark add-client-foundation D4 as superseded.
- add-mail-calendar's interactive Microsoft sign-in, which also used a WebView2 window, moves to the system browser with a loopback redirect, the same way Keycloak sign-in works.

Out of scope:
- Any change to Thread, Run, Approval or policy behaviour.
- New screens. The Automations editor, Skills and Knowledge screens arrive with their own changes and are built natively from the start.
- Syntax highlighting in code blocks.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/desktop-host`: On-Demand UI Window rendered natively, with no web engine; a window footprint budget.
- `client/work-inbox`: Markdown rendering of model output.

## Impact

- **Roadmap:** inserted as 6a, after add-browser-use and before add-image-generation (6b), which shows its images in the native window. add-client-foundation and add-work-inbox must be archived before this change, because it modifies their capabilities.
- **Superseded decisions:** add-client-foundation D4 (UI technology) and the WebView2 parts of D12; add-work-inbox D7 (TypeScript string bundles, axe-core) and D14 (UI tests in jsdom against a mock host). add-work-inbox task 1.1 (list prototype in WebView2) and 7.2 (axe-core gate) are replaced by tasks of this change.
- **Forward references:** add-mail-calendar (11) changes its interactive sign-in to the system browser; add-computer-use (8), add-skills (9), add-scheduled-and-remote-tasks (10), add-knowledge-graph (12) and add-meeting-capture (13) build their screens with the native toolkit.
- **Code removed:** `client/ui/` (Svelte app, its tests and the UI mock dev server), the UI resource packer, `WebView2Loader.dll` and the WebView2 SDK header.
- **Code added:** a native UI toolkit and the inbox screens under `client/src/ui/`, and a vendored Markdown parser.
- **Effort:** the largest client change so far. It is staged (design.md D12), and the WebView2 UI keeps working until the native window covers the whole client/work-inbox spec.
