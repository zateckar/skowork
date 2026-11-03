# Tasks

## 1. Validation spikes

- [ ] 1.1 Send OTLP/HTTP protobuf from a minimal C++ encoder to the organization observability stack through a relay in the Node.js control plane. Verify the records appear with correct attributes and the encoder adds ≤ 300 KB to the binary

## 2. Telemetry catalogue

- [x] 2.1 Create `telemetry/catalogue.yaml` and generators for the C++ and TypeScript allowlists. Verify CI fails when code emits a field not in the catalogue

## 3. Client telemetry (client/telemetry)

- [x] 3.1 Implement level handling (off/metadata/content) driven by policy, including the content indicator. Verify with unit tests per level, and a UI test for the indicator
- [x] 3.2 Implement secret redaction (built-in patterns plus policy.telemetry.redaction_patterns) and URL-to-domain reduction. Verify with unit and fuzz tests over the redaction input, including a policy-supplied pattern
- [x] 3.3 Emit the network enforcement level and policy decision records into metadata telemetry. Verify with a test that a denied call produces a record with rule id and policy version but no path
- [x] 3.4 Implement the encrypted offline buffer with cap, ordered flush and discard counter. Verify with an offline/online integration test
- [x] 3.5 Implement crash capture for core and workers with the filtered minidump, user view and delete. Verify with an induced worker crash test
- [x] 3.6 Implement the Telemetry viewer in settings. Verify that it lists sent and buffered records for the last 30 days

## 4. Telemetry ingest (control-plane/telemetry-ingest)

- [x] 4.1 Implement the authenticated OTLP endpoint with an installation-to-user check and level enforcement. Verify with integration tests for valid, spoofed and over-level batches
- [x] 4.2 Implement HMAC pseudonymization, forwarding with a 24-hour buffer, retention jobs and rate limiting. Verify with tests for target outage, retention expiry and the rate-limit response
- [x] 4.3 Load-test the ingest at 30k clients × 1 batch/min. Verify p99 accept latency < 500 ms and document sizing in design.md

## 5. Admin console (control-plane/admin-console)

- [x] 5.1 Implement identity-provider sign-in (Authorization Code with PKCE), role mapping and authorization for the five roles. Verify with tests for each role, including the read-only works-council-reviewer, and for denial
- [x] 5.2 Implement the policy administration views on top of the policy-service API, showing its validation errors, diff, simulation sources and rollout adoption (control-plane/policy-service Rollout Adoption). Verify with end-to-end tests of publish and adoption
- [x] 5.3 Implement the model/budget/rate-limit, identity (deny list), Remote task (dispatch rules), MCP catalog and fleet views on the respective service APIs, with placeholders for the skill review queue, service principals and issuer rate limits, whose services come with add-skills and add-scheduled-and-remote-tasks. Verify each view with an end-to-end test against a seeded environment
- [x] 5.4 Implement the append-only audit store, viewer and export, plus individual-level access with mandatory reason. Verify that the audit store rejects updates and deletes, and that each access is recorded
- [x] 5.5 Implement aggregate dashboards with the k=5 suppression floor. Verify that small Groups are suppressed

## 6. Integration

- [x] 6.1 End-to-end pilot rehearsal: a client at the metadata level runs a session. Verify that the admin sees aggregates but no content, the user sees the same records in the Telemetry viewer, and the auditor's access is audited
- [x] 6.2 Run `openspec validate --all --strict` and fix any findings

## Workflow follow-up

- Archive with `openspec archive add-telemetry-and-admin --yes` after review.
- Before production rollout (organizational, not a technical gate): hand the metadata catalogue to the organization-level approval process (data protection officer, works council). Development and limited testing do not wait for it.
