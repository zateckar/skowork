# Design

## Context

See proposal.md, section "Why", for the motivation. The work builds on these existing pieces:

- identity and the fleet registry from add-client-foundation;
- Run and tool events from client/agent-runtime;
- policy publication from control-plane/policy-service.

The list of collected fields is a generated, reviewable artefact (see D2), not something left to individual code paths. The organization-level approval process (data protection officer and works council) can therefore check exactly what is collected before production rollout. That approval is organizational and is not enforced technically, per the project constraints.

## Goals / Non-Goals

**Goals:**
- One telemetry schema shared by the client, the ingest and the dashboards, with a field allowlist for each telemetry level.
- A minimal console that is enough to run the MVP pilot, extended by later changes.

**Non-Goals:**
- Building our own observability backend. Telemetry is forwarded to the organization stack.
- Productivity analytics of any kind.

## Decisions

### D1. OpenTelemetry with a closed field allowlist
- **Choice:**
  - The client emits OTLP over HTTP/protobuf, with the payload encoded by our own minimal C++ encoder.
  - Every attribute name must appear in a generated allowlist keyed by telemetry level. Any attribute not on the list is dropped at emit time.
  - The ingest re-applies the same allowlist, giving defence in depth.
  - All client telemetry is OTLP log records: one event record per Run, tool call, model call, Approval, policy decision, error, start, footprint sample or crash, named by the record's event name and described by catalogue attributes. Counts, durations and rates are aggregated from these events in the control plane, so the client sends no OTLP metrics or traces, and the ingest decodes one message type.
- **Why:** this keeps metadata-only collection enforceable and reviewable.
- **Alternatives considered:**
  - The official opentelemetry-cpp SDK: about 3–5 MB extra and heavy dependencies, which conflicts with the footprint budget.
  - A custom JSON event format: would require custom ingest in the organization stack.
  - OTLP metrics and traces from the client as well: a second and third message type to encode, decode and allowlist, for data the events already carry.

### D2. Metadata catalogue as a reviewed artefact
- **Choice:** `telemetry/catalogue.yaml` lists every span, metric and log field with its level and purpose. Code generators produce the C++ allowlist for the client and the TypeScript allowlist for the control plane from it.
- **Process:** catalogue changes go through normal code review. The current catalogue is the document handed to the organization-level approval process before production rollout.
- **Alternative considered:** documenting the fields in prose. Rejected because prose drifts from the code.

### D3. Offline buffer
- **Choice:** the offline buffer is the table `telemetry-buffer.db` (encrypted SQLite, from client/local-storage) and works as FIFO with a size cap. Batches are flushed every 60 seconds or every 500 records, whichever comes first.
- **Alternative considered:** in-memory only. Rejected because it loses data while users are off VPN.

### D4. Crash reports
- **Choice:**
  - Capture: `MiniDumpWriteDump` with `MiniDumpFilterMemory | MiniDumpWithoutOptionalData`, restricted to thread stacks and module lists. Workers are dumped by the core process through their Job Object.
  - Symbolication: server-side from the private symbol store.
  - Dump type: `MiniDumpFilterMemory | MiniDumpWithoutOptionalData | MiniDumpFilterModulePaths`. Module paths would otherwise carry the user's profile folder.
  - Writing: an unhandled-exception filter in each process passes the exception to a writer thread started at launch, because the crashing thread may have no stack left (a stack overflow).
    - The core writes `<data root>/crashes/core-<time>.dmp` and imports it into the encrypted buffer at the next start.
    - A worker, which runs with a restricted token, cannot open files in the data root. The core creates an empty dump file for each worker it starts and passes the file to the worker as an inherited handle (`--crash-dump <handle>`). When the worker ends, a file that is no longer empty is a crash report; an empty one is deleted. "Through their Job Object" thus becomes "through a handle from the core". It needs no debugger rights, and a dump can still be written after the job's memory limit is hit.
    - When crash reports are disabled (policy.telemetry.crash_reports false, or level off), the core passes no handle and its own filter writes nothing.
  - Storage and upload: the raw dump exists unencrypted only until it is imported. The import keeps a summary (exception code, faulting module, thread count, module names) and the dump in `telemetry-buffer.db`. A report is held for one hour for the user's review, then posted to `/api/v1/telemetry/v1/crashes` as octet-stream with its metadata in headers. The control plane checks the installation the same way as for logs, encrypts the dump with AES-256-GCM under a key derived from the telemetry secret, and keeps it for 90 days.
- **Alternative considered:** WER/Watson upload. Rejected because the dumps would go to a Microsoft endpoint, which is not acceptable.

### D5. Pseudonymization
- **Choice:** HMAC-SHA256 with a key held in the control plane's secret store, rotated yearly. Re-identification is an audited admin-console function.
- **Alternative considered:** hashing without a key. Rejected because it can be reversed by brute force over the directory.

### D6. Console technology
- **Choice:**
  - Backend: a module of the control-plane process (Node.js with TypeScript and Express), with an Admin API.
  - Frontend: a React and TypeScript single-page app, served by the same process (control-plane stack, decided 2026-10-22).
  - Authorization: the five admin roles are assigned in the control plane (control-plane/admin-console, Admin Roles) and checked on every request. There are five roles; works-council-reviewer is read-only and limited to the records of individual-level access.
  - One table maps each console view to the roles that may open it, and the server checks the same table on the view's API:

    | View | Roles |
    |---|---|
    | dashboards, fleet | platform-admin, security-auditor |
    | policy | platform-admin, security-auditor (read), group-owner (owned Groups) |
    | models, identity, remote tasks, MCP catalog | platform-admin |
    | skill review queue | skill-reviewer, platform-admin |
    | audit log, individual-level data | security-auditor |
    | access records | security-auditor, works-council-reviewer |

    platform-admin qualifies for every management API but not for the audit log or individual-level data. `GET /api/v1/admin/me` returns the views of the signed-in admin, and the single-page app shows only those. A user with no admin role gets 403 on every admin API, `/me` included. Every 403 under `/api/v1/admin` is written to the audit log as `admin.refused`, with the method and path but no query string.
  - Individual-level access: the auditor states the reason in the request (`reason`, 1–500 characters). The access is written to the audit log (category `individual-access`, with the auditor, the user concerned and the reason) before any data is read. The access records view lists exactly these events.
  - Behaviour owned elsewhere: the console calls the administration APIs of policy-service, identity, llm-gateway (through policy keys), mcp-catalog, skill-registry, task-dispatch and fleet-registry. It holds no business rules of its own besides role authorization, audit and aggregation.
- **Alternatives considered:**
  - Blazor (.NET): a second runtime and UI stack beside the Node.js control plane and the client's C++ and Svelte stack. Rejected on 2026-10-22 with the control-plane stack decision.
  - A low-code admin tool: weak role-based access control and audit support.

### D7. Aggregation floor
- **Choice:** dashboards suppress any cell with fewer than 5 distinct users (k-anonymity floor 5), so aggregates cannot identify individuals.

### D9. Append-only audit store
- **Choice:** the `audit` table has SQLite triggers that abort every UPDATE and DELETE, so no code path, including a future retention job, can change it without a migration that drops the triggers in review. Export is a security-auditor download of the filtered events as JSON Lines or CSV, and the export itself is audited.
- **Alternative considered:** a hash chain over the events. It detects changes after the fact but does not stop them, and nothing in the pilot verifies it. It can be added later without changing the store.

### D10. End-to-end tests of the console
- **Choice:** `npm test` builds the single-page app with Vite and drives it in headless Chrome through the DevTools protocol, using the `ws` client the control plane already has. The browser runs with a temporary profile against a control plane on loopback, and signs in at an in-test identity provider that serves discovery and a token endpoint. The tests are skipped when no Chrome or Edge is found.
- **Alternatives considered:**
  - Playwright. It downloads its own browsers into the user profile, outside the repository.
  - Rendering components in Node. That would not test sign-in, routing or the calls to the API.

### D11. Rollout adoption
- **Choice:** when the policy service issues a bundle it also stores the Group and guardrail versions that went into it. An installation runs the version recorded with its user's bundle when the bundle version it reports to the fleet registry equals that bundle's version; otherwise it is counted as not yet updated.
- **Alternative considered:** clients reporting the version of each Group. That would change the fleet registry's protocol for something the server already knows.

### D8. Matcher for policy redaction patterns
- **Choice:** the client matches policy.telemetry.redaction_patterns with its own small matcher (client/src/telemetry/pattern.*). It parses the ECMAScript syntax subset without back-references and lookaround, compiles the pattern to an instruction program capped at 2,000 instructions, and runs it as a Pike VM. A search is one pass over the text, so time is linear in the text times the program size. Every application has a step budget. If the budget runs out, the whole text becomes the redaction marker, so a pattern can cost time but never lets text through unredacted. Matches are leftmost-first, as in ECMAScript, and empty matches are not replaced. A differential fuzz target compares the results with std::regex on short inputs.
- **Alternatives considered:**
  - std::regex. Rejected because the fuzz target (task 3.2) found `(a|b)*******c` running for 214 s on 110 characters with libstdc++'s backtracking executor. Its polynomial mode still took 291 s on 8 KB, because it restarts the search at every position, and std::regex cannot bound the work.
  - RE2. Rejected because it brings Abseil into the client build for one feature.

## Implementation Notes

- Catalogue (task 2.1): `telemetry/catalogue.yaml` lists the resource fields (sent once per batch), the attributes with type, level and purpose, and the events with the attributes each may carry. `npm run telemetry:catalogue` in control-plane/ checks it (dotted names, known types and levels, every event attribute defined, every attribute used) and writes `client/src/telemetry/catalogue.gen.h` and `control-plane/src/telemetry/catalogue.gen.ts`; CI runs it with `--check`. The client names fields only through the generated enums, so code that emits a field missing from the catalogue does not compile; ctest builds a probe with an unknown field and expects the build to fail, next to the same probe with a known field that must build. Types are string, integer, number, boolean and domain; a domain attribute is reduced to the host name before it is stored, and strings are cut to 256 characters at metadata level and 8,000 at content level.- Ingest (tasks 4.1, 4.2): `POST /api/v1/telemetry/v1/logs` takes OTLP protobuf (at most 1 MB and 1,000 records) with the user's access token. The batch's `skowork.installation_id` must be registered to the token's user in the fleet registry; otherwise the batch is refused with 403 INSTALLATION_MISMATCH and a security audit event. The ingest re-applies the generated allowlist: unknown events and fields are dropped and counted, content fields from a sender whose effective policy.telemetry.level is not content are dropped and counted as violations, values of the wrong type are dropped, and domain fields are reduced to the host again. Accepted records are stored per user, installation and Groups (for the dashboards and individual-level access), and deleted after `SKOWORK_TELEMETRY_RETENTION_DAYS` (30). When `SKOWORK_TELEMETRY_FORWARD_URL` is set, each accepted batch is queued for forwarding with `enduser.id` and `host.name` added to its resource, both replaced by keyed pseudonyms when `SKOWORK_TELEMETRY_PSEUDONYMIZE` is on. A forwarding loop sends the queue in order and stops at the first failure; the queue keeps batches for `SKOWORK_TELEMETRY_FORWARD_BUFFER_HOURS` (48, never less than 24). Pseudonyms are the first 80 bits of HMAC-SHA256 under a key derived (HKDF) from `SKOWORK_TELEMETRY_SECRET`, which production must set; the mapping is stored for audited re-identification. Each installation has a token bucket (6 batches a minute, burst 20); excess batches get 429 RATE_LIMITED with Retry-After. Operational counters are at `GET /api/v1/admin/telemetry/stats` (platform-admin).

Client telemetry (tasks 3.1, 3.3 and 3.4) lives in client/src/telemetry. TelemetryService stores each record in telemetry-buffer.db, which is encrypted with its own key. Each record is filtered at the level in force when it is emitted, and filtered again at send time. Content captured at the content level is therefore not sent after the policy lowers the level to metadata. Batches hold up to 500 records and go when 500 are waiting or 60 seconds have passed. The ingest's retry-after is honoured. When the cap drops records, a telemetry.buffer record carrying the discard count leads the next batch. Sent records stay 30 days for the Telemetry viewer. RunCollector is a runtime hook that emits run.finished, tool.call and model.usage. A DecisionLog observer emits policy.decision and approval.resolved. Policy rule ids lose their paths: filesystem rules keep only the rule key, and folder grants become grant. A failed Run carries only the typed code from the start of its reason. The status bar and the tray tooltip show the content indicator while the content level is in force. The Telemetry viewer (task 3.6) is the page #/settings/telemetry, reached from the Privacy section of Settings and from the content indicator. It shows the level, the Group that set it, the policy version and the counters. Below them it lists the records of the last 30 days, newest first, each with its state (sent or waiting) and every field as it is or will be sent.

Ingest sizing (task 4.3): `npm run load:telemetry -- --clients 30000 --minutes 3` measures the ingest.

- Setup:
  - The script seeds one registered installation per client.
  - It signs a token per client and starts the control plane in a child process on a file database.
  - Each client sends one OTLP batch of 25 metadata records a minute, spread evenly over the minute (500 batches/s).
  - Every request is fully authenticated and stored.
- Workstation result: an i7-12700 with 20 threads, Node 26, a single control-plane process.

  | Load | Batches | p50 | p95 | p99 | Max |
  |---|---|---|---|---|---|
  | 30k clients (500/s) | 90,000, all accepted | 4.9 ms | 12.9 ms | 19.4 ms | 55 ms |
  | 1.5× (750/s, first minute) | all accepted | 10 ms | | 200 ms | |
  | 2× (1,000/s) | | | | 2.5 s | |

  At 1,000/s the process saturates. One process therefore carries the 30k clients with about 1.5× headroom.
- The first run, with SQLite's default `synchronous = FULL`, reached a p99 of 7 s. A CPU profile showed about half the time blocked in commit syncs: each batch's transaction, plus each new session's first-use row and its audit event. The database now runs WAL with `synchronous = NORMAL`. A process crash loses nothing; a power loss can lose the last commits, but never corrupts the database.
- Storage: a stored record takes about 450 bytes with its indexes. The test's worst case of one full batch a minute from every client (about 1 billion records a day) cannot be kept for 30 days. Real clients send only what they record. Size the disk as users × records a day × retention days × 450 B; for example, 30k users × 300 records × 30 days ≈ 120 GB. Lower `SKOWORK_TELEMETRY_RETENTION_DAYS` where the organization stack keeps the long-term copy.
- Scaling past one process: the control plane is one process on one SQLite file, and SQLite cannot be shared between replicas. Beyond about 45k clients at this rate, either the ingest moves to a process of its own or the store moves to a server database. That is a later decision, made only if the pilot's volumes call for it.

Admin console (tasks 5.1–5.5):

- Server: `control-plane/src/admin/`:
  - `authz.ts` holds the view table (D6), `requireView` and the admin gate, which audits every 403.
  - `routes.ts` serves `/me` with its views, the audit viewer and export (`/audit`, `/audit/export?format=jsonl|csv`), individual-level access (`/individual/telemetry`, `/individual/crashes/:id`, `/individual/pseudonyms/:p`, each needing `reason`), `/access-records` and `/dashboards`.
  - `dashboards.ts` aggregates the installations, telemetry records and gateway usage of the period per Group. A Group with fewer than 5 active users shows nothing. Breakdown cells (tools, versions) under 5 users are merged into one count that names none of them.
- Management views: the policy, identity, fleet and MCP catalog views call those modules' existing administration APIs. `GET .../adoption` reports Rollout Adoption (D11). The Policy Diff accepts `none` as a side, so a draft can be diffed before anything is in force.
- Single-page app: `control-plane/admin/src`, with one file per view under `views/`. Policy validation errors are shown in a table of the document's fields, each error on the field it names. The skill review queue, service principals and issuer rate limits are placeholders until add-skills and add-scheduled-and-remote-tasks fill them.
- Tests:
  - `test/admin.test.ts` covers roles and refusals, the append-only store, export, individual access and dashboards.
  - `test/admin-e2e.test.ts` runs the D10 browser tests: PKCE sign-in for every role, publishing with a pilot rollout and adoption, and the models, identity, Remote task, MCP and fleet views.

## Risks / Trade-offs

- [Developers add a telemetry field without review] → The catalogue drives the allowlist. Unknown fields are dropped and counted, and CI fails if the code references a field that is not in the catalogue.
- [Content level enabled for production users before organization-level approval] → This is an organizational control, not a technical gate. Content level is set only by an explicit policy.telemetry.level change, which is audited in control-plane/policy-service. The org guardrail can cap the level at metadata, and affected users see a persistent indicator.
- [Crash dumps contain user data despite filtering] → Dumps are stored encrypted, deleted after 90 days, readable only by the security-auditor role, and listed in the user's Telemetry viewer.
- [OTLP volume from about 30k clients overloads the ingest] → Batching, a per-installation rate limit and horizontal scaling. Load test in task 4.3.

## Migration Plan

Not applicable. There is no existing system.

## Open Questions

- Which observability stack (Elastic, Splunk or Grafana) is the forwarding target? Only the deployment configuration depends on this, because the interface is OTLP.
