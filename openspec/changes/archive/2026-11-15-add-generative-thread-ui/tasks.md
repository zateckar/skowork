# Tasks

## 1. Validation spike

- [x] 1.1 Compare the proposed catalog and event flow with A2UI and dsh-genui, and record the protocol decision and trade-offs in the design; verify the catalog covers the delta specs.
- [x] 1.2 Exercise a provisional v1 schema against at least 40 representative prompts across two available organization gateway model configurations; record validity, usefulness, failure cases and latency without secrets, and resolve failures or unavailable configurations as specified in D5 before implementation.
- [x] 1.3 Prototype representative native table, chart, diagram and controls on the development machine pinned to four logical cores; record p95 rendering, scrolling, incremental memory and keyboard/UI Automation results against D5 targets, and resolve any failed targets in the design. The production i5 baseline remains separately unverified.

## 2. Surface contract and validation

- [x] 2.1 Define the versioned schema, complete catalog, limits, example surfaces and model instructions in the implementation; verify valid examples and invalid versions, IDs, types, sizes, references and data shapes with parser unit tests.
- [x] 2.2 Add a fuzz target for surface discovery, JSON validation and size/depth limits; verify malformed and adversarial input neither crashes nor executes or fetches content.
- [x] 2.3 Restrict activation to complete top-level blocks in durable assistant Thread events and preserve readable fallback elsewhere; verify streaming truncation, cancellation, quoted examples, user messages, tool results and human Chat messages.

## 3. Native presentation and accessibility

- [x] 3.1 Render composable sections, cards and sortable/filterable tables with state retained on refold; verify local interaction makes no model request and large input respects limits.
- [x] 3.2 Render labeled bar/line charts and directed diagrams with accessible text/table alternatives; verify data, units, cycles, layout limits, keyboard access and UI Automation roles.
- [x] 3.3 Implement accessible buttons, selections, switches, text fields and sliders with explicit review and submit; verify focus, values, theme, localization and no submission on local changes.
- [x] 3.4 Resolve surface Citation IDs through the existing answer Citation mechanism; verify valid references open and unknown IDs never appear as verified links.

## 4. Durable feedback and Run routing

- [x] 4.1 Add encrypted revision-bound draft and local exploration state with a versioned, backward-compatible storage upgrade and Thread deletion; verify stream refold, navigation, restart, superseded revisions, old-database upgrade and deletion after downgrade.
- [x] 4.2 Extend ask_user and Question persistence for validated structured forms while retaining free-text responses; verify subagent and parent resumption, restart and stale or cross-Run Question rejection.
- [x] 4.3 Route submitted answer surfaces to the same Thread as explicit user messages and offer queue/steer when required; verify completed, running, interrupted, cancelled and deleted cases.
- [x] 4.4 Persist idempotent submission and resolution or enqueue atomically, validate visible values server-side, and display retryable failures; verify concurrent duplicate activation, restart retries, storage failure and rollback without repeated acceptance.
- [x] 4.5 Ensure generated actions cannot resolve Approval items or directly execute Tools; verify a generated request requiring approval waits for the existing Approval card and cannot bypass policy.

## 5. Integration and rollout

- [x] 5.1 Gate prompting, rendering and interaction behind the disabled-by-default feature switch; verify disabled mode, existing Threads, older clients and downgrade retain readable content.
- [x] 5.2 Run end-to-end acceptance with the configured real organization gateway, covering a visual comparison, local exploration, a structured Question, a follow-up and a policy-gated Tool; record actual outcomes and limitations.
- [x] 5.3 Re-run performance, accessibility, high-contrast and localization checks on the integrated client on the development machine pinned to four logical cores against D5 targets; record measurements and fix regressions before enabling, and report the unverified production i5 baseline.
- [x] 5.4 Validate all OpenSpec changes strictly and reconcile this change with the in-flight native UI, Thread, runtime and feed specifications; verify no capability ownership or requirement conflicts remain.

## Workflow follow-up

- Archive this change after every task is complete and strict validation passes, then inspect the merged specifications for coherence.
