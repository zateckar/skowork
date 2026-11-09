## 1. Directory

- [x] 1.1 Implement the admin directory (D2). Verify with an API test that installation users, direct members, administrators, Group owners, deny-listed and bootstrap users and the seen identity-provider roles are listed

## 2. Admin console

- [x] 2.1 Shared building blocks: Records, Dialog, ask(), Picker, Tabs and the visual system (D1, D4). Verify in the browser with keyboard only: open a dialog, pick from a list, Escape
- [x] 2.2 Identity, Fleet and Remote tasks views (D3). Verify in the browser and with the end-to-end test
- [x] 2.3 Policies and Models & quotas views (D3). Verify in the browser and with the end-to-end test
- [x] 2.4 LLM connections, MCP catalog, Skill review and Team knowledge views (D3). Verify in the browser and with the end-to-end test
- [x] 2.5 Audit log, Individual access, Access records and Dashboards (D3). Verify in the browser and with the end-to-end test

## 3. Acceptance

- [x] 3.1 Check the console in the browser, light and dark: Identity, Policies, Models & quotas, LLM connections, MCP catalog and the deny list by hand as platform-admin. Every view and role, including security-auditor, through the end-to-end test. Run the control-plane tests
- [x] 3.2 Run `openspec validate --all --strict`
