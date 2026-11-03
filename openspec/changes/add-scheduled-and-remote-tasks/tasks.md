# Tasks

## 1. Trigger framework (client/scheduler)

- [x] 1.1 Add Trigger, Occurrence and attempt tables with leases in `app.db`. Verify with crash-recovery tests that an expired lease with a `running` Run returns to pending and counts a failed attempt, while an Occurrence whose Run is `interrupted` keeps its Run and Approval after restart
- [x] 1.2 Implement the Occurrence state machine with 3 attempts, backoff, dead state and cancelled state (user Stop, issuer cancel). Verify every transition, using fault injection on Run failure
- [x] 1.3 Implement idempotent Run creation keyed by Occurrence id. Verify that a double-fire test produces exactly one Run
- [x] 1.4 Treat firings suppressed by Pause background work (client/desktop-host) as missed. Verify catch-up on resume with a clock-simulation test

## 2. Trigger kinds (client/scheduler)

- [x] 2.1 Implement schedule Triggers with time-zone cron, DST handling, wake detection and catch-up. Verify with clock-simulation tests for sleep, DST transitions and the catch-up window
- [x] 2.2 Implement file Triggers with change notifications, debounce and startup reconciliation. Verify a single Occurrence per stable file
- [x] 2.3 Implement session Triggers (logon, unlock) and the plug-in interface for the mail, calendar (offsets from event start/end) and meeting (detected / transcript ready) kinds, with unavailable-kind rejection. Verify that a mail Trigger is rejected before add-mail-calendar is available

## 3. Execution and safety (client/scheduler)

- [x] 3.1 Mark Occurrence Runs as Unattended, keep the Occurrence running while its Run is interrupted, and release and re-acquire concurrency slots. Verify the mail.send scenario with a policy-enforcement approve decision
- [x] 3.2 Implement policy.scheduler.max_concurrent_runs (within policy.llm.max_concurrent_runs) and the enabled_kinds, max_triggers and min_interval_minutes limits. Verify each limit with tests
- [x] 3.3 Implement thread modes new and append with origin links. Verify that append mode reuses the Thread and marks it unread

## 4. Management UI and tools (client/scheduler)

- [x] 4.1 Build the Automations section content (list, per-kind editors, Run now, history with Thread links, Remote task history). Verify with UI tests, including opening a result from history
- [x] 4.2 Implement the tasks.list (read), tasks.create, tasks.update, tasks.delete and tasks.run_now (write-local) tools with the "created by agent" label. Verify that an agent-created Trigger passes the policy decision and appears in Automations

## 5. Remote tasks (client/scheduler, control-plane/task-dispatch)

- [x] 5.1 Implement the dispatch API (create, status, cancel), service principal authentication (Service Principal Tokens), dispatch-rule authorization, Group fan-out and rate limits. Verify with API tests (`npm test`, node:test, on SQLite) for every requirement scenario
- [x] 5.2 Implement delivery through the fleet outbox, the client messages (remote-task-claim, remote-task-accept, remote-task-decline, remote-task-status) with refusal for non-claiming installations, remote-task-claimed for losers, claim refusal after cancel or expiry, and remote-task-cancelled to all target installations. Verify the two-installation test, where only one runs, and a claim after cancellation
- [x] 5.3 Implement client acceptance (a standalone Thread with a client-raised Question, trusted issuers, always-accept option, withdrawal on remote-task-claimed or remote-task-cancelled), execution as an Occurrence with untrusted task content, status reporting with the dispatch status mapping, and issuer cancellation. Verify end to end against a test control plane, including the injected-approval prompt scenario
- [x] 5.4 Implement result sharing controlled by policy.scheduler.remote_share_results, with callbacks only to URLs on the callback allowlist (a control-plane server setting managed in control-plane/admin-console) and audit. Verify that results are withheld when sharing is not allowed and that non-allowlisted callback URLs are rejected
- [x] 5.5 Fill the admin console's placeholders for service principals (register and retire, with the APIs each may call), issuer rate limits and the callback allowlist (control-plane/admin-console Identity Administration and Remote Task Administration). Verify each view with an end-to-end test through the console

## 6. Integration

- [ ] 6.1 End-to-end test: a CI service principal sends a Remote task to a developer. The task is accepted through a trusted issuer, runs as Unattended, is interrupted by an approve decision on an external action, the user approves it, the task succeeds and the issuer sees the status
- [x] 6.2 Run `openspec validate --all --strict` and fix any findings

## Workflow follow-up

- Archive this change with `openspec archive add-scheduled-and-remote-tasks --yes` after review, then check that the merged specs read coherently.
