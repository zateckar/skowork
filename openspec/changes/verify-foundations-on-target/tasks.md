# Tasks

Each task names the foundation task it came from. Record results in the owning capability's archived design (`openspec/changes/archive/*-<foundation>/design.md`) or, for a new decision, in a new change.

## 1. Organization identity (from add-client-foundation, add-knowledge-graph)

- [ ] 1.1 (add-client-foundation 1.1) Prove Authorization Code with PKCE against the organization Keycloak realm from an organization laptop. Cover system-browser sign-in with the loopback redirect, a 5-minute access token and refresh-token rotation. Verify that reusing a rotated refresh token fails and that the browser SSO session gives a prompt-free second sign-in. Already verified on the development workstation on 2026-10-22: sign-in, 300 s tokens, rotation and revocation.
- [ ] 1.2 (add-client-foundation 5.1, real-token part) Verify that the control plane accepts a real access token from the organization Keycloak and maps its roles to Groups. The locally signed token tests pass; see task 5.1 in add-client-foundation.
- [ ] 1.3 (add-knowledge-graph 1.3) Verify a Graph user and manager lookup with the delegated User.Read.All scope (admin consent; add-knowledge-graph design D5) through the organization Entra app registration.

## 2. Network (from add-client-foundation)

- [ ] 2.1 (add-client-foundation 1.2) Prove WinHTTP WebSocket over TLS through the corporate proxy/PAC, both on site and over VPN. Verify a 24-hour connection with reconnects stays healthy.

## 3. Baseline and organization laptops (from add-client-foundation, add-knowledge-graph)

- [ ] 3.1 (add-client-foundation 1.3) Measure the signed release client on the baseline laptop with the window closed. Verify an idle working set ≤ 60 MB and an install size within the client/desktop-host Footprint Budgets. On 2026-11-19, `check-footprint.ps1 -InstallOnly` reported 28.89 MB against the 30 MB budget on the development workstation.
- [ ] 3.2 (add-client-foundation 1.4) Migrate a test profile containing a DPAPI-wrapped encrypted SQLite database with the organization profile-migration tool. Verify the database opens on the target machine.
- [ ] 3.3 (add-knowledge-graph 1.1) Benchmark retrieval on the baseline laptop at 400k and 1M chunks with acme-llm-embedding vectors and acme-llm-reranker through the gateway. Test the `dimensions` parameter and compare retrieval quality at 1024 and 4096 dimensions on a cs/de/en query set. Verify p95 hybrid query latency including both gateway calls stays under 1.5 s. Record storage per chunk and the chosen dimension. `client/tests/knowledge/kg_bench.cpp` measures the local part.

## 4. Second Windows account (from add-client-foundation)

- [ ] 4.1 (add-client-foundation 3.4) Verify that a connection to the client's named pipe from a different Windows user is refused, and that the running client has no listening sockets. Pipe.DaclGrantsOnlyTheCurrentUser verifies the user-only DACL locally.
- [ ] 4.2 (add-client-foundation 4.2) Verify that a copied encrypted database cannot be opened under another Windows account. Dpapi.RoundTripBoundToPurpose and Dpapi.RejectsGarbage pass locally.

## 5. Real LLM gateway (from add-agent-runtime, add-knowledge-graph)

- [ ] 5.1 (add-agent-runtime 1.3) Load-test the organization LLM API through the gateway with 50, 200 and 500 concurrent streaming sessions, and measure embedding throughput in chunks per second. Record p95 time-to-first-token, tokens/s and chunks/s, and size the gateway rate limits and per-backend capacity from them.
- [ ] 5.2 (add-knowledge-graph 1.2) Evaluate entity and relation extraction by the on-premise `small` model on a labelled cs/de/en sample of 200 chunks. Record precision and recall.
- [ ] 5.3 (add-agent-runtime 5.1) Before each release, run the eval suite end to end (client runtime to gateway to on-premise model) locally with `eval_runner --threshold`. CI cannot reach the organization LLM API (add-agent-runtime design D8, D10). Verify the pass rate meets the threshold.

## 6. Signing and publishing (from add-client-foundation)

- [ ] 6.1 (add-client-foundation 2.1, signing part) Verify that CI with the code-signing certificate configured produces a signed `Skowork.exe` and `skowork-worker.exe`.
- [ ] 6.2 (add-client-foundation 2.5) Verify that a push to `main` that passes CI replaces the rolling `dev` pre-release (desktop zip and app runtime package with checksums) and the `main` control-plane image, and that a pull request publishes nothing.

## 7. Desktop input and end-to-end runs (from add-client-foundation, add-work-inbox, add-knowledge-graph)

These runs need desktop input or an identity-provider sign-in. Ask the user before each run.

- [ ] 7.1 (add-work-inbox 6.1) Click a toast notification and verify its deep link opens the correct Thread.
- [ ] 7.2 (add-work-inbox 8.1) End-to-end: a user-started Run in a background Thread raises an Approval while the window is closed. Verify a toast appears, the Approval card opens with provenance, and the Run completes with the Thread unread.
- [ ] 7.3 (add-client-foundation 6.1) End-to-end on an organization laptop against the organization Keycloak. Verify logon, tray, sign-in, registration, online in the fleet registry, refresh over an hour, and that revocation locks the client.
- [ ] 7.4 (add-knowledge-graph 6.2) End-to-end on the baseline laptop with the mock mailbox and a document set. Ask about a project and verify a cited answer drawn from mail, documents and earlier Threads.
