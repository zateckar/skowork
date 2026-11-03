# Proposal

## Why

Much organization work happens in web applications that have no API or MCP server, such as portals, intranet tools and SaaS consoles. The agent needs to operate a real browser in order to read and act in them. Chrome is the managed organization browser. From version 136, Chrome refuses remote debugging on the user's default profile, so we need a governed, dedicated browser setup.

## What Changes

- Add browser use through the Chrome DevTools Protocol:
  - a dedicated, persistent Chrome profile under the data root;
  - controlled over a debugging pipe, never a port;
  - a check of the enterprise Chrome policy at startup, with a clear failure message.
- Add page perception: accessibility/DOM snapshots with element references as the primary mode, and screenshots for the vision model role.
- Add browser actions:
  - navigate, click, type, select, scroll, wait and screenshot;
  - JavaScript evaluation, which is gated as destructive;
  - downloads into the quarantine folder.
- Add governance:
  - every browser request (navigation, redirect and subresource) checked against policy.network egress rules;
  - the agent never types passwords, and sign-in is handed over to the user;
  - a visible window for attended Runs, and headless mode for Unattended runs only if policy allows it;
  - organization sign-in through the OAuth identity provider, whose session the dedicated profile keeps.

Out of scope:
- Controlling the user's everyday Chrome profile or its tabs. An extension bridge is a possible later change.
- Microsoft Edge.
- Browsers other than Chrome.
- Defeating CAPTCHAs or bot detection. The agent hands these over to the user.

## Capabilities

### New Capabilities
- `client/browser-use`: governed control of a dedicated Chrome instance by the agent, covering perception, actions, sign-in hand-over and downloads

### Modified Capabilities
- `client/agent-runtime`: tool results can carry images, sent once with the vision role (Image Tool Results)

## Impact

- **Roadmap:** change 6 of 13 (MVP). It depends on:
  - add-client-foundation (data root);
  - add-agent-runtime (tools, vision model role, image input sizing, client-raised Questions, provenance);
  - add-policy-and-sandbox (decisions, policy.network egress rules, egress proxy);
  - add-work-inbox (Question presentation; the Connections settings section);
  - add-core-tools-and-mcp (the quarantine downloads folder from client/core-tools).
- **Forward references:** headless mode for Unattended runs only matters once client/scheduler (change 10) exists.
- **New code:** a `client/` browser tool module with a CDP client.
- **External dependencies:** Google Chrome installed and managed by the organization. The organization Chrome policy allows remote debugging (confirmed 2026-10-21).
