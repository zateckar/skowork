# Design

## Context

- **What already exists:** the push channel and fleet registry (add-client-foundation); Runs, interrupts and Unattended handling hooks (add-agent-runtime); and policy decisions (add-policy-and-sandbox).
- **The detection sources come later:** the mail, calendar and meeting kinds depend on detection sources from later changes. This change defines the Trigger framework they plug into.
- **Pizza-bot comparison:** pizza-bot's trigger service is the reference for the Occurrence state machine. It lacked run history linked to Threads.

## Goals / Non-Goals

**Goals:**
- Triggers fire reliably and never twice, across sleep, crashes and restarts.
- Unattended work never takes an external or destructive action without the user.
- Systems such as Jira or CI can hand work to a user's agent through a simple, audited API.

**Non-Goals:**
- Server-side execution of agent Runs. Runs always execute on the user's laptop.
- Firing while logged off.
- Multi-step workflows across users.

## Decisions

### D1. Durable Occurrences in `app.db`
- **Choice:** Triggers, Occurrences and attempts are tables in `app.db`. Before an Occurrence starts its Run, the state change from pending to running is committed together with a lease (installation id, expiry). On startup, the scheduler checks running Occurrences whose lease has expired. Lease expiry applies only when the Occurrence's Run is still in state `running`: such an Occurrence goes back to pending and counts as a failed attempt. An Occurrence whose Run is `interrupted` keeps its Run and its persisted Approval or Question (client/agent-runtime). It is not re-leased or retried, so a restart never creates a duplicate Run or Approval.
- **Why:** this gives at-least-once with idempotent Run creation keyed on the Occurrence id, which makes the effect exactly-once.
- **Alternative considered:** Windows Task Scheduler. Rejected because it needs OS task registration, is visible and fiddly, and conflicts with the "runs only while logged on inside the client" requirement.

### D2. Time handling
- **Choice:** cron is evaluated in the Trigger's IANA time zone using the ICU time-zone data that ships with Windows (`icu.dll`). DST gaps are skipped and overlaps fire once. Wake-from-sleep is detected through `PBT_APMRESUMEAUTOMATIC` and a monotonic clock check.
- **Alternative considered:** UTC-only cron. Rejected because users think in local time.

### D3. File Triggers
- **Choice:** `ReadDirectoryChangesW` on the permitted folders, debounced until the file has been stable for 5 seconds. A full rescan on startup reconciles changes missed while the client was not running.
- **Alternative considered:** polling. Rejected because of the CPU cost against the idle budget.

### D4. Unattended interrupts release the slot
- **Choice:** the scheduler marks the Run as Unattended. All decisions are made by client/policy-enforcement through the runtime's `BeforeToolCall` hook, and the scheduler adds no rules. When the runtime reports that the Run is interrupted, the Occurrence stays `running` and the scheduler releases the Run's concurrency slot. When the Run resumes, it re-acquires a slot and may wait for one.
- **Why:** an Unattended run that is waiting must not block other Triggers.
- **Alternative considered:** failing the Run. Rejected because work already done would be lost.

### D5. Dispatch service
- **Choice:** a module of the one control-plane process (Node.js with TypeScript and Express), with SQLite tables (`node:sqlite`) for tasks, member tasks and transitions. Service principals use service-principal tokens issued by control-plane/identity (Service Principal Tokens requirement in specs/control-plane/identity).
  - **Alternative considered:** a separate task service with PostgreSQL (the earlier plan), rejected on 2026-10-22 with the control-plane stack decision. Task volume is well within one process, and the fleet outbox already provides store-and-forward.
- **Delivery and claiming:**
  - Delivery uses the fleet-registry outbox (`remote-task` push).
  - The client claims a task through a `remote-task-claim` message on client/control-channel. The claim is a conditional update, `status = delivered → claimed` with the installation id. A task that is already claimed, cancelled or expired cannot be claimed. Losing installations receive `remote-task-claimed`.
  - After the claim, the status moves to `awaiting-acceptance`. For trusted issuers it moves straight to `accepted`. The client's `remote-task-accept` or `remote-task-decline` moves it to `accepted` or `declined`.
  - `remote-task-status` messages carry `running` and the final Occurrence state. They map to dispatch statuses as follows: succeeded→succeeded, failed/dead→failed, cancelled→cancelled.
  - On issuer cancellation or expiry, `remote-task-cancelled` is pushed to every installation of the target user. The client withdraws any pending acceptance Question.
  - The callback allowlist is a control-plane server setting managed in control-plane/admin-console.
- **Alternative considered:** a message broker such as Kafka or RabbitMQ. It is not needed at the expected volume, since the fleet outbox already provides store-and-forward.

### D6. Remote tasks are not Triggers
- **Choice:** an accepted Remote task becomes an Occurrence with no Trigger, linked to the Remote task. That reuses the state machine, retries and history without creating a "remote" Trigger kind. The Run's origin is the Remote task (client/agent-runtime).
- **Alternative considered:** a built-in "remote" Trigger per user. Rejected because it blurs the glossary and lets users "edit" something that is controlled centrally.

### D7. Issuer ids, the remote-task messages and their replies
- **Issuer ids:** `user:<user name>` for a person and `service:<name>` for a service principal. These are the values policy.scheduler.remote_trusted_issuers and policy.scheduler.remote_share_results list, the issuer the client shows, and the key of the per-issuer rate limit.
- **The remote-task push payload:** `{task_id, parent_id, issuer: {id, kind, name}, prompt, skill, attachments: [{name, content_type, size, data}], expires_at, provenance: {source_kind: "remote", ref: <issuer id>, untrusted: true}}`. The prompt and attachments are content, nothing else in the payload carries authority (Request Content Is Untrusted). Fields an issuer adds to its request are not copied.
- **Replies:** the client messages use the push message id of the remote-task push as `ref`. Task dispatch answers each one with `{type: "reply", ref, payload: {message, task_id, status}}`, or with the existing error frame `{type: "error", ref, payload: {code, reason, message}}`. A claim repeated by the installation that holds it is answered as a success, because the client outbox may resend it after a reconnect. A claim of a task the user's policy trusts (remote_trusted_issuers) moves straight to `accepted`.
- **Delivered** is set when the fleet registry has delivered the push to one of the user's installations; it is derived from the outbox when a task is read or claimed.
- **Expiry** applies to tasks that are not yet running: queued, delivered, claimed, awaiting-acceptance or accepted. A running task ends with the client's final status or by cancellation.

### D8. Service principals and server settings
- **Tokens:** a service principal is a client of the organization identity provider using the client-credentials grant. Its tokens are verified like user tokens (issuer, signature, audience, lifetime cap). The token's authorized party (`azp`) must be a registered, not retired service principal whose registration lists the API being called (today only `tasks`). A client-credentials token (the `client_id` claim, or a `service-account-` user name) from any other client is refused with 403. A registered service principal's token on a user API is refused with 403. Both refusals are audited.
- **Storage:** the registrations (client id, name, APIs, registered by, retired at), the per-issuer rate limits and the callback allowlist are control-plane tables edited through the admin console, not environment settings, because administrators change them at run time.
- **Group members** for fan-out are the users assigned to the Group in the control plane plus the users whose installations last reported the Group (role mappings are only known from tokens).
- **Callbacks** are POSTed once per final member status, with a 10 second timeout, to a URL that starts with an allowlisted prefix (scheme, host and path). A request naming any other callback URL is rejected when it is created.

### D9. Client scheduler structure
- **Portable core, polled.** `wb_scheduler` holds the tables, the Occurrence state machine, cron evaluation over an injected time-zone interface and the Trigger limits. A `tick()` (from the host's periodic tick, and right after resume from sleep) fires due Triggers, starts pending Occurrences while slots are free and reads the state of the Runs it started. The core reads time only from the injected clock, so clock-simulation tests drive it without timers.
- **Runs.** An Occurrence's Run is started with origin trigger_occurrence (ref: the Occurrence id) or remote_task (ref: the Remote task id). Before starting, the scheduler looks for a top-level Run with that origin; one found is adopted, which makes Run creation idempotent across a crash between the lease commit and recording the Run id. The scheduler counts its queued and running Unattended runs against policy.scheduler.max_concurrent_runs (at most the runtime's max_concurrent_runs). The runtime itself also holds resumed Unattended runs while that many run (`RuntimeConfig.unattended_slots`), so a resumed Run waits for a slot.
- **Leases** are renewed by the tick while the Run is running. At startup the runtime's recovery has already failed the Runs that were running ("interrupted by shutdown"), so an Occurrence whose lease expired and whose Run failed counts a failed attempt and goes back to pending with backoff (1, then 5 minutes). An interrupted Run keeps its Occurrence running.
- **Time zones** come from Windows' `icu.dll` (`ucal_*`), loaded at run time. A local time in a DST gap is skipped; a local time that occurs twice resolves to its first instance only, so it fires once.
- **Missed firings.** A schedule Trigger keeps its next firing time. A tick that finds it in the past fires it as scheduled when it is less than 2 minutes late and background work was not paused at the time; otherwise one catch-up Occurrence is created when the latest missed firing is inside the catch-up window, and the others are recorded in the history as skipped (at most 20 per catch-up).
- **Withdrawing a Question:** `AgentRuntime::withdraw_standalone(action_id, reason)` resolves a standalone Question with `{kind: "withdrawn", reason}` and emits action.resolved, so the Thread shows why it is no longer asked.
- **Notifications.** The scheduler announces an Occurrence's outcome itself, through the inbox's notification policy, only when the Trigger has notify on and only for the final state (succeeded, or dead after the last attempt). The inbox therefore does not raise completed or failed notifications for Runs with origin trigger_occurrence, so a retried attempt does not notify.
- **File Triggers on Windows.** `FindFirstChangeNotificationW` per folder, polled without waiting by the tick, plus a full listing every 5 minutes and at startup. A listing is reported only after it has been unchanged for 5 seconds (debounce, files still being written). The scheduler fires once per path, size and modification time; the first listing of a new Trigger only records what is already there.
- **Remote tasks on the client.** `RemoteTasks` keeps each task in `remote_tasks` (claiming, refused, awaiting, accepted, running, then the final state, or declined or cancelled), keyed by task id, and correlates the server's answers by the push id the client messages carry (D7). The control channel passes reply and error frames to it (`ControlChannel::on_reply`); the push and reply handlers run on the app's worker pool, not on the channel thread. A Thread is created only after a won claim. "Always accept from this issuer" is stored on the client (`remote_trusted_issuers`); the server does not know it, so the client answers `awaiting-acceptance` for such an issuer with remote-task-accept at once. The accepted task's prompt, with the list of its attachments (stored as Thread attachments with untrusted provenance), starts the Run through `StartOptions.untrusted`: the runtime wraps the text in the Run's untrusted-data delimiter after a short trusted preamble naming the issuer, and records the message's provenance as untrusted, so every policy decision of the Run sees it as untrusted context. The Occurrence listener sends remote-task-status for running and for the final state, with the Run's last answer as the result only when policy.scheduler.remote_share_results lists the issuer. After an issuer cancellation no status is sent. The client half is verified against a scripted dispatcher that applies task dispatch's client-message rules to two installations, each with its own runtime and app.db; the server half is verified by its API tests; the whole path with a real control plane and identity provider is task 6.1.
- **Source plug-ins.** mail, calendar and meeting Triggers are accepted only while a source for the kind is registered (`Scheduler::register_source`). A source validates conditions and calls `Scheduler::source_event` with a firing key; the calendar source computes the offset times itself. Without a source the kind is unavailable ("mail source unavailable").

## Risks / Trade-offs

- [Trigger storms (many file events, many mail matches) drain the battery and LLM quota] → Debouncing, policy.scheduler.min_interval_minutes, policy.scheduler.max_triggers, the concurrency limit, the LLM budget of control-plane/llm-gateway, and the background deferral from client/desktop-host.
- [Remote tasks are used to push unwanted work or social engineering] → Dispatch rules, first-time acceptance, issuer identity shown on every task, task content treated as untrusted, Unattended decisions by client/policy-enforcement, rate limits and audit.
- [Result sharing leaks user data] → Off by default, per issuer, through policy only, and every disclosure is audited.

## Open Questions

- Which integrations (for example Jira, CI, ServiceNow) get service principals first? This is deployment configuration only.
