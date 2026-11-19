## 1. Validation spike

- [x] 1.1 With the pinned node.exe on the baseline hardware: node:sqlite under --permission limited to one folder, listening on a request handling over stdin/stdout frames, fetch to an internet host failing inside the sandbox, start time and memory. Record the results in design.md

## 2. Client

- [x] 2.1 App manager: folders and manifest, dependency refusal, start in the sandbox with the permission flags, stdin/stdout frames (D2), idle stop, 3-app limit, logs (D1, D2, D5). Verify with tests
- [x] 2.2 Loopback listener with launch token and session cookie, Host check, proxy to the app over its frames (D3). Verify with tests: no cookie refused, token single-use, wrong Host refused
- [x] 2.3 MCP bridge with manifest list, confirmation, read-risk check, policy.apps.mcp and policy decisions (D4). Verify with tests against a fake MCP server
- [x] 2.4 Tools apps.create, apps.list, apps.logs, apps.open, apps.stop, apps.delete, and policy.apps.mode, policy.apps.mcp in the policy schema. Verify with runtime tests
- [x] 2.5 Apps view: list, open, stop, show folder, delete, change with the agent, strings in en, de, cs. Verify with hidden-window UI tests and the a11y check
- [x] 2.6 Runtime package: fetch script for the pinned node.exe into the repository, package layout and size check against the budget. Verify the size

## 3. Acceptance

- [x] 3.1 With the real LLM: ask for a presentation app and for a view over a connected MCP server; open both in the browser. Record the result in design.md
- [x] 3.2 Run `openspec validate --all --strict`
