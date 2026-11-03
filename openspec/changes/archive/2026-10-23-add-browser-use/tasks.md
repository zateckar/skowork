# Tasks

## 1. Confirmation on a managed laptop

- [x] 1.1 Remote debugging is allowed by organization policy (confirmed 2026-10-21). On a workstation with the real enterprise Chrome policy, confirm it works: launch Chrome with a dedicated profile and `--remote-debugging-pipe`, complete a CDP handshake, then open a page behind the organization identity provider (OAuth). Verify that all three work and that the identity provider's sign-in page raises the sign-in hand-over, and record the Chrome version
- [x] 1.2 Run the agent with the real main model on public sites (Wikipedia, Google, Amazon): search, read a page and fill a form. Record the snapshot sizes and the outcome of each task in design.md, and verify that the Wikipedia tasks succeed

## 2. Browser process and CDP (client/browser-use)

- [x] 2.1 Implement the Chrome discovery, version check and policy check with clear failure reasons. Verify that browser tools are withdrawn when the policy key is set to false
- [x] 2.2 Implement the dedicated-profile launch with the inherited pipe and the CDP transport. Verify no listening port with a socket-listing test, and fuzz the CDP message parser
- [x] 2.3 Implement the lifecycle (lazy start, idle close, no close while the user is interacting) and visible/headless mode selection. Verify both modes in tests

## 3. Perception and actions (client/browser-use)

- [x] 3.1 Implement browser.snapshot with refs, pruning and untrusted provenance. Verify the stale-reference error after a page change
- [x] 3.2 Implement browser.screenshot sized per model-access image input, held in memory for the Run, and written to disk only with retain_screenshots. Verify that no screenshot remains after the Run by default
- [x] 3.3 Implement navigate, click, type, select, scroll, wait, tabs and back. Verify against a local test site covering each action
- [x] 3.4 Implement the declared risk classes, submit and cross-site classification, and the destructive browser.eval gate. Verify the classification against a fixture set of forms in cs, de and en
- [x] 3.5 Implement image tool results in client/agent-runtime. Verify that the image goes with the vision role in the next request only and that no image data is stored in the Thread

## 4. Governance (client/browser-use)

- [x] 4.1 Implement egress enforcement for every browser request through CDP Fetch interception plus the egress proxy. Verify that a denied navigation and a denied subresource send no request
- [x] 4.2 Implement credential-field refusal and the sign-in hand-over as a client-raised Question (Continue/Stop). Verify that typing into a password field is refused and that the Run resumes after the user chooses Continue
- [x] 4.3 Route downloads to the quarantine folder. Verify mark-of-the-web on a downloaded file

## 5. Integration

- [x] 5.1 End-to-end test on a public site with the real main model: the agent opens Wikipedia, reads a table, fills the search form and asks for approval on submit. Verify that the Approval shows the page as provenance
- [x] 5.2 Run `openspec validate --all --strict` and fix any findings

## Workflow follow-up

- Archive this change with `openspec archive add-browser-use --yes` after review, then check that the merged specs read coherently.
