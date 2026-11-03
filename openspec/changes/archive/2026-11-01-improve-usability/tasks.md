## 1. Admin console

- [x] 1.1 Fix the layout (D3), add labels, accessible names and confirmations for destructive actions. Verify in a 1280 px and a 1920 px browser window that no card overlaps and that cancelling a confirmation changes nothing
- [x] 1.2 Implement the structured policy editor (D2) next to the raw JSON view. Verify with a component test that toggling computer use and adding a host updates the JSON, and in the browser that a published Group policy reaches a running client

## 2. Client

- [x] 2.1 Implement Withheld Tool Explanation in find_tools and for direct calls (D1). Verify with runtime tests for computer use and shell switched off, a tool denied by rule, and an unrelated search
- [x] 2.2 Add the Policy page (D4) with sources and Refresh. Verify with a hidden-window UI test that reads the computer-use row through UI Automation
- [x] 2.3 Add the Files section with the folder picker and grant revoke. Verify with a hidden-window UI test and a runtime test that the agent can read a granted folder
- [x] 2.4 Add UI for folder rename and delete, queued-message edit, per-Run usage totals in the Activity log, withdrawing a published Skill and item exclusions in the knowledge settings. Verify each with a hidden-window UI test

## 3. Acceptance

- [x] 3.1 Run the stack (dev-stack.ps1, a development client, the admin console) and go through every admin and user scenario of this change by hand. Record the result in design.md
- [x] 3.2 Run `openspec validate --all --strict`
