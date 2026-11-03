# Tasks

## 1. Validation spikes

- [ ] 1.1 Load 20 public agentskills.io / Claude skills with a prototype parser. Verify that every skill parses and that unknown frontmatter is preserved
- [ ] 1.2 Prototype the GitLab-backed registry: create a repository, tag a version and serve it through the registry API with a Workbench token. Verify that clients need no GitLab credentials

## 2. Client skill loading (client/skills)

- [x] 2.1 Implement the SKILL.md parser and validation (required fields, kebab-case name, semantic version, optional folders). Verify with unit tests and a fuzz target for the frontmatter parser
- [x] 2.2 Implement the five load tiers, precedence, shadowing display and locked org skills. Verify with tests for each conflict case
- [x] 2.3 Implement the tool-scope intersection, the default Skill tool set, and the skill-declared decision floor (interrupt_on, risk) with client/policy-enforcement. Verify that a skill cannot gain a tool or remove an approval, and that a call above the declared risk is approved
- [x] 2.4 Implement the network field (no-network sandbox mode for scripts) and suggested triggers (offer only, created after user confirmation). Verify that a network-none script's requests through the proxy are refused and that no Trigger exists without confirmation

## 3. Running skills

- [x] 3.1 Expose skill names and descriptions to the main agent, and run selected skills as subagents. Verify that the Activity log shows the delegation in an end-to-end test
- [x] 3.2 Implement `/name` invocation with completion in the composer. Verify with a UI test that the explicitly invoked skill is used
- [x] 3.3 Implement script execution via shell.run with the policy.skills.script_runtimes check. Verify that a PowerShell script runs sandboxed and that a disallowed runtime is refused

## 4. Authoring

- [x] 4.1 Implement the skill editor (frontmatter, body, files, validation). Verify with UI tests that a valid private skill is created and saved
- [x] 4.2 Implement "Save as skill" drafting with personal-data parameterisation. Verify on sample Threads that names, email addresses and paths become parameters
- [x] 4.3 Implement skill test runs in labelled test Threads. Verify that the test uses the draft's tool scope
- [x] 4.4 Implement publishing from the client (shared with named users, group, org under policy.skills.publish) and the user's published versions with their review state. Verify that an org publish shows In review and that a registry refusal is shown with its message

## 5. Skill registry (control-plane/skill-registry)

- [x] 5.1 Implement storage, immutable versioning, scopes and publish authorization against policy.skills.publish. Verify with tests for republish and scope rejection
- [x] 5.2 Implement the scanner (secrets, PowerShell AST commands and URLs, declared tools and MCP servers, injection patterns). Verify against a labelled test corpus, including a hard reject for secrets
- [x] 5.3 Implement the org review state machine, self-review block, ES256 signing and public import with source and licence, and fill the admin console's skill review queue view (scan results, diff against the previous version, approve or reject with a comment, public import for platform-admins; control-plane/admin-console Skill Review Queue). Verify with end-to-end tests through the admin-console review queue
- [x] 5.4 Implement the distribution API, skills-changed and skill-revoked pushes, revocations in sync responses, and aggregate usage counts. Verify that a revocation removes the skill from a connected client on push, and from an offline client at its next sync

## 6. Client installation and lifecycle

- [x] 6.1 Implement sync (start, reconnect, push, every 24 h), install by digest, signature verification, pinning, auto-update by policy and rollback. Verify with tests for signature mismatch and rollback
- [x] 6.2 Implement requires_mcp gating and revocation handling during a running subagent. Verify with tests for a missing server and mid-Run revocation

## 7. Integration

- [ ] 7.1 End-to-end: user A saves a Thread as a skill, shares it with user B, publishes to org, a reviewer approves, and the skill is revoked. Verify each step on two test clients
- [x] 7.2 Run `openspec validate --all --strict` and fix any findings

## Workflow follow-up

- Archive with `openspec archive add-skills --yes` after review.
