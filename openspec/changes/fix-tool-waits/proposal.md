# Proposal

## Why

On 2026-11-10 the product owner reported two tool calls that never finish:

- **A Skill waits forever.** The agent calls `use_skill` for the installed Skill "internal-policy-lookup", and no answer comes. A Skill runs as a child Run. When the child Run needs an Approval, the Approval belongs to the child Run. The feed draws no card for it, so the user cannot approve it. Nothing ends the wait.
- **A local app write hangs.** The user approves a file write while the agent builds a Local app. The write then waits until the tool times out. The Local app manager holds one lock while an app that does not start is retried, so the write's policy check waits up to 10 seconds per browser request.

## What Changes

- **Child Run waits.** An Approval raised by a child Run in an attended Run expires after 30 minutes unanswered. The call is rejected and the child Run continues. A child Run in an attended Run ends as failed 60 minutes after it started, waiting included.
- **Child Run cards.** The Approval and Question cards of a child Run appear in the Thread feed under the subagent entry that started it, and can be answered there. Answering resumes the child Run and its parent.
- **Local app start.** Starting a Local app no longer blocks folder access, app creation or app changes. An approved file write to an app folder completes within 2 seconds, even while an app is starting.

Out of scope:
- Expiry of Approvals raised by top-level Runs. They stay open until the user answers.
- Expiry for Unattended runs. Add-agent-runtime keeps their Approvals open with no timeout.
- Making the wait limits policy keys. They are fixed values in this change.
- Cool-down after a failed app start, and changes to how Skills are selected or run.
- Changes to policy decisions, the sandbox, the MCP bridge or the app server's security.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/agent-runtime`: adds Child Run Approval Expiry and Child Run Time Limit.
- `client/work-inbox`: adds Child Run Action Items.
- `client/local-apps`: adds Folder Access During App Start.

No requirement is changed or removed. The owning requirements of the forward references listed under Impact are not yet merged into `openspec/specs/`, so this change adds new requirements instead of modifying them.

## Impact

- **Roadmap:** 40 [Hardening].
- **Depends on** (forward references to changes not yet archived; each is listed here as the config requires):
  - add-agent-runtime: Subagents, Approval Interrupts, Unattended Approval Waiting, Cancellation.
  - add-work-inbox: Approval Card, Question Card.
  - add-skills: Skills Run As Subagents.
  - add-local-apps: App Folder, App Lifecycle. This change is complete but not yet archived.
- **Compatible with** fix-long-thread-model-calls. It modifies other requirements of client/agent-runtime; this change only adds requirements, and does not change the Run states that it modifies.
- **Code:**
  - client/src/runtime/runtime.cpp: child Run expiry and time limit.
  - client/src/inbox/inbox_service.cpp: `threads.feed` returns the events of interrupted child Runs.
  - client/src/shell/feed.cpp: child Action items are placed under the subagent entry.
  - client/src/apps/apps.cpp and apps.h: the start no longer holds the manager lock.
  - client/src/app/app.cpp and runtime.h: the periodic call of `expire_child_waits`.
- **Data:** none. No stored format changes and no control-plane change.
- **Client:** the Thread feed shows child Run cards, and Skill and subagent waits end on their own.
