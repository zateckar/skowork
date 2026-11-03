# Proposal

## Why

Much valuable agent work is not started by the user typing. Examples:
- a daily digest;
- preparing for an upcoming meeting;
- reacting to a new file or email;
- a task handed over by a colleague, or by a system such as Jira or CI.

Users need these Runs to happen reliably and safely, and their results should arrive in the inbox like any other Thread.

## What Changes

- Add the client scheduler:
  - Trigger kinds: schedule, file, mail, calendar, meeting and session.
  - An Occurrence state machine for Trigger firings and accepted Remote tasks: pending, running, then succeeded, failed, dead or cancelled. Failed attempts are retried up to 3 times with backoff.
  - Catch-up of one missed run within a policy window.
  - Run history per Trigger, linked to the resulting Threads.
  - A thread mode per Trigger: new or append.
- Mark Occurrence Runs as Unattended runs. Their tool calls are decided by client/policy-enforcement, and interrupted Runs release their concurrency slot.
- Add the `tasks.*` tools so the agent can create and modify Triggers when the user asks.
- Add the content of the Automations settings section, with "Run now", next run and last run.
- Add the client side of Remote tasks: claim, first-time acceptance, execution as an Occurrence, and status reporting.
- Add control-plane task dispatch:
  - an API through which people and service principals request Remote tasks for a user or Group;
  - authorization rules and first-time consent by the target user;
  - delivery through fleet push with store-and-forward and expiry;
  - optional result callback;
  - audit.

Out of scope:
- Running Triggers while the user is logged off.
- Cross-user shared Threads.
- Workflow orchestration across several users.
- The detection logic behind the mail, calendar and meeting Trigger kinds. That lives in add-mail-calendar and add-meeting-capture; this change defines only the Trigger framework they plug into.

## Capabilities

### New Capabilities
- `client/scheduler`: Triggers, Occurrences, execution of Unattended runs and accepted Remote tasks, run history, the `tasks.*` tools and the Automations section
- `control-plane/task-dispatch`: requesting, authorizing, delivering and tracking Remote tasks for users and Groups

### Modified Capabilities
- `control-plane/identity`: adds service principals, whose client-credentials tokens may submit Remote tasks. These requirements moved here from add-client-foundation.

## Impact

- **Roadmap:** change 10 of 13 (Managed). It depends on:
  - add-client-foundation (push channel and its client-to-server messages, fleet registry, control-plane/identity);
  - add-agent-runtime (Runs, interrupts, client-raised Questions, concurrency and unattended run limits);
  - add-policy-and-sandbox (Unattended decisions, untrusted content rule, dispatch rules in control-plane/policy-service);
  - add-work-inbox (origin links, Action view, standalone Action items, Automations section);
  - add-telemetry-and-admin (admin console for issuer rate limits, the registration of service principals and the callback allowlist; service principals themselves are defined here);
  - add-skills (the optional skill of a Trigger or Remote task).
- **Later changes that plug in:**
  - the mail and calendar Trigger kinds become functional with add-mail-calendar (change 11), which publishes mail events and event start and end times;
  - the meeting kind becomes functional with add-meeting-capture (change 13), which publishes "meeting detected" and "transcript ready".
  Until then those kinds are rejected as unavailable.
- **New code:**
  - the `client/` scheduler module and the Automations UI;
  - the `control-plane/` task-dispatch service and its public REST API for integrations.
