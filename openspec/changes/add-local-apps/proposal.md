# Proposal

## Why

On 2026-10-31 the product owner asked that the agent can build small web applications for the user alone, using Node.js, HTML, CSS and SQLite without external dependencies, for example a presentation or a view over data of a connected MCP server, each app in its own folder. Today the agent can write files, but nothing runs or serves them, and an app cannot reach MCP data without the agent.

## What Changes

- A local app is a folder with a manifest, server code for the bundled Node.js runtime, static files and its own SQLite database. Apps use only built-in Node.js modules and their own files.
- The agent creates and changes apps with new apps.* tools and writes their files with the files.* tools.
- The client starts an app on demand in a contained process without network access, and serves it only to the user through a loopback address opened in the user's browser.
- Apps read data of connected MCP servers live through the client: only read-risk tools named in the manifest, confirmed by the user, decided by policy as for the user.
- An Apps view lists the apps with open, stop, show folder, delete and change with the agent.
- The Node.js runtime ships as an optional signed package, like the speech-to-text package.
- Policy keys policy.apps.mode (on | off) and policy.apps.mcp (off | read).

Out of scope:
- Sharing apps with other users or publishing them; apps reachable from other machines.
- npm packages, downloads at build or run time, native add-ons.
- Write or destructive MCP tools from apps.

## Capabilities

### New Capabilities
- `client/local-apps`: app folders, runtime, serving to the user only, MCP bridge, lifecycle, Apps view, apps.* tools.

### Modified Capabilities
- `client/desktop-host`: Footprint Budgets (adds the optional app runtime package).

## Impact

- **Roadmap:** 19, after add-user-messaging. Depends on add-policy-and-sandbox (tool-sandbox, policy-enforcement), add-core-tools-and-mcp (files tools, mcp-client), add-work-inbox and add-native-ui.
- **Code:** client/src/apps (manager, proxy, bridge, tools), client/src/shell (Apps view), packaging of the runtime package, strings in en, de, cs.
- **Data:** an apps folder in the user's profile, one subfolder per app.
- **Glossary:** adds Local app; adds apps.* to the tool ids and apps to the Policy bundle keys.
