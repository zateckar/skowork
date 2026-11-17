# Tasks

## 1. Validation spikes and dependencies

- [ ] 1.1 Verify Graph-mock source identity, revisions, tombstones, page/cursor atomicity and resync in `client/src/mail/mail_sync.cpp`, `mail_store.cpp` and `scheduler/mail_sources.cpp`; reproduce crash without duplicate suggestions and queue overflow with a visible gap while source sync continues; update specs before proceeding if the source cannot support the contract.
- [ ] 1.2 Map registered Tools, parallel calls and Child Run effect and reconciliation capabilities, including Graph, MCP, API connectors, browser, computer and shell; reproduce crash after dispatch and before result in `client/tests/runtime/runtime_test.cpp`, with unknown as the default. Do not infer Graph send idempotency without a provider-backed test.
- [ ] 1.3 Verify the in-flight scheduler, runtime, mail and inbox changes satisfy their relevant contracts and integration prerequisites; run their focused suites and the open scheduler remote-task integration scenario before claiming end-to-end readiness. Reconcile deltas and archive order before archive.
- [ ] 1.4 Before implementing recovery, create a disposable prototype profile with the proposed migration in an isolated Windows user or VM and test it with the actual prior client to prove an enforceable database version gate prevents unsafe downgrade. Release binaries ignore development isolation flags; verify profile, registry and network isolation before launch. If no such gate is available, revise the migration design before implementation and do not release recovery.

## 2. Event intake and triage

- [ ] 2.1 Add disabled-by-default enablement, selected mail/calendar scope, user responsibilities, baseline time, visible overflow and expiry counts, and disable/delete controls in the native Settings UI; verify restart persistence, access revocation and keyboard access.
- [ ] 2.2 Atomically commit source pages, triage records or overflow gap, and resumable cursor with encrypted identity, revisions and tombstones; test crash at each commit boundary, continued source sync at the 1,000-item limit, user-acknowledged baseline, restart, cursor expiry/resync, retention and visible gaps using Graph mock and injected clock.
- [ ] 2.3 Connect eligible mail/calendar changes after source filtering, and invalidate intake and in-flight results on disable, deletion, exclusion, scope or access changes; verify race cases, transient offline behavior and source-derived content removal through the local-storage forget cascade.
- [ ] 2.4 Batch and classify via the gateway `small` role with no Tools; verify unavailable role, schema, bounded IDs and revisions, duplicate/missing outputs, retry/backoff, request spacing, deferral, unknown outcomes and prompt injection; fuzz untrusted classifier responses. Use in-test fakes for deterministic faults and the real organization LLM API for local acceptance.
- [ ] 2.5 Create, update, withdraw and dismiss source-linked system-origin suggestion Threads, atomically claim the revision to start one Run with untrusted provenance, and retain dismissal per revision; verify concurrent start, edits during classification, deletion, source access checks and no Run or Tool execution from classification alone.

## 3. Effect-aware recovery

- [ ] 3.1 Implement a durable operation ledger and conservative Tool recovery registry in `client/src/runtime/`, including migration and legacy unknown handling; correct connector failure messages that falsely assert an ambiguously dispatched request was not made; test storage failure, ambiguous timeout and crash before intent, after intent, after dispatch, after effect and after result persistence, including parallel Tool calls and Child Runs.
- [ ] 3.2 Implement an atomically claimed new continuation Run linked to the shutdown-failed Run and reuse recorded results; test duplicate activation and crash, protocol-valid history and compaction, original history, queued ordering, interrupted Approvals, current policy, original origin and carried budgets.
- [ ] 3.3 Implement a durable standalone Question before any continuation model call, a live-Run interrupt for ambiguous timeouts, and a persistent guard requiring explicit authorization of each potentially effectful call while outcomes remain unknown; test stop, continue-with-unknown, changed arguments or Tool, child delegation, double resolution, repeated restart, cancellation, revoked policy, edited approvals and untrusted content. Existing retry paths must not bypass the guard.
- [ ] 3.4 Gate scheduler startup on runtime recovery, then hold an Occurrence with an unknown effect without a slot or retry and settle it from its uniquely bound continuation or user stop; test startup race, second restart, origin lookup, attempt accounting, failed continuation, safe failures, user/issuer cancellation, revocation and one final notification.
- [ ] 3.5 Add only provider-specific reconciliation supported by the spike, with a stable operation id and read-only verification; use a controlled provider fake to test success, absence, ambiguity, timeout and crash between reconciliation and commit. Leave unsupported Tools unknown.

## 4. Background work visibility

- [x] 4.1 Expose bounded sanitized Run and Child Run activity and durable state transitions from `client/src/runtime/`; test concurrent children, progress updates, interruption, cancellation, restart and redaction of secrets and raw Tool output.
- [ ] 4.2 Project progress, pending steering, Action links and recovery choices into `client/src/inbox/` and `client/src/shell/`; verify live feed ordering, navigation, keyboard/UI Automation access, stale actions and restart reconstruction.
- [x] 4.3 Reuse existing queue, steering and Stop paths and communicate delivery at the next safe boundary; test a long-running Tool, queued messages, parent/child cancellation and a pending Question.

## 5. Integration and release evidence

- [ ] 5.1 Run an isolated end-to-end Graph-mock scenario: eligible event, suggestion, explicit start, approval, injected shutdown around an external effect, restart and safe resolution; assert no unauthorized action, duplicate effect or lost provenance.
- [ ] 5.2 Measure disabled idle overhead, burst backpressure, gateway call rate and responsiveness on the baseline Windows machine; verify design targets and existing desktop-host budgets. Test an actual prior client against an upgraded disposable profile; if a version gate cannot prevent unsafe downgrade, do not release recovery.
- [ ] 5.3 Run focused runtime, scheduler, mail, inbox and native UI suites, parser fuzzing, applicable integration checks, and `openspec validate --all --strict`; record environment and results without treating skipped live systems as verified.

## Workflow follow-up

- After all tasks and prerequisite changes are completed and reviewed, archive with `openspec archive improve-proactive-durable-work --yes` and reread merged specs for coherence.
