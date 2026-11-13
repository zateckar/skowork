# Tasks

## 1. Validation spikes

- [ ] 1.1 Prove Authorization Code with PKCE against the organization Keycloak realm from an organization laptop: system-browser sign-in with the loopback redirect, a 5-minute access token, and refresh-token rotation. Verify that reusing a rotated refresh token fails and that the browser SSO session gives a prompt-free second sign-in. (Partly verified 2026-10-22 on the development workstation with `dev/scripts/oidc-probe.mjs`: sign-in, 300 s tokens, rotation, revocation; results in design.md D7.)
- [ ] 1.2 Prove WinHTTP WebSocket over TLS through the corporate proxy/PAC, from on-site and from VPN. Verify a 24-hour connection with reconnects stays healthy
- [ ] 1.3 Measure a C++ skeleton built with GCC/MinGW (tray, pipe, WinHTTP, encrypted SQLite, idle WebView2 closed) on the baseline laptop. Verify idle working set ≤ 60 MB and install size ≤ 25 MB
- [ ] 1.4 Migrate a test profile containing a DPAPI-wrapped encrypted SQLite database with the organization profile-migration tool. Verify the database opens on the target machine
- [x] 1.5 Prove the GCC/MinGW toolchain covers the roadmap's platform APIs: WebView2 through the SDK header and `WebView2Loader.dll`, a WinRT call through the ABI headers (Windows.Graphics.Capture), UI Automation, WASAPI process-loopback activation with hand-written declarations, and an AppContainer process launch. Verify each with a minimal program that runs on Windows 11, and record any gaps in design.md

## 2. Repository and build

- [ ] 2.1 Create the `client/` CMake solution for GCC/MinGW with presets (debug, release, sanitize, fuzz) and vendored, pinned third-party sources with a manifest of versions, hashes and licences. Verify a clean offline CI build produces a signed `Skowork.exe` with no non-system DLL dependencies except `WebView2Loader.dll`
- [x] 2.2 Create the `control-plane/` Node.js TypeScript service (Express, node:sqlite with migrations, `ws`) with the Identity and Fleet modules, a React admin console shell served by the same process, a Dockerfile, and `dev/scripts/dev-stack.ps1`, which runs it from the repository. Verify that `npm test` passes locally and in CI
- [ ] 2.3 Add CI quality gates (GCC warnings as errors and hardening flags, clang-tidy, `-fanalyzer`, a Linux job running the portable libraries' unit tests under ASan/UBSan and an AFL++ fuzz smoke run, footprint check). Verify a deliberate budget violation fails the pipeline
- [x] 2.4 Add GitHub Actions release workflows that publish two artifacts for a version tag: the control plane as a container image and the desktop client as a zip package with its checksum. Verify a tag build produces both
- [ ] 2.5 Publish a dev build from CI on every push to `main` that passes (design D9 Published artifacts): the control plane image tagged with the dev version and `main`, and the desktop zip and app runtime package with their checksums on the rolling `dev` pre-release. Verify that a push to `main` replaces the `dev` pre-release and the `main` image, and that a pull request publishes nothing

## 3. Desktop host (client/desktop-host)

- [x] 3.1 Implement per-user start at logon (HKCU Run key) with repair, single instance and argument hand-off over the named pipe. Remove the private-use URI scheme registration. Verify with an automated test that launches twice
- [ ] 3.2 Implement the tray icon states, menu and confirmation on Quit. Verify through UI automation tests for each state
- [x] 3.3 Implement the on-demand WebView2 window with the embedded SPA shell (About, Status, Settings placeholders). Verify that closing the window terminates the WebView2 processes
- [ ] 3.4 Implement the named-pipe JSON-RPC API with the user-only DACL and remote-client rejection. Verify a different-user connection is refused and no listening sockets exist
- [x] 3.5 Implement Pause background work (indexing and embedding, transcription, mail/calendar sync and Trigger firings) and background deferral. Deferral covers indexing and embedding, background transcription and sync on battery saver (unless `policy.ui.background_on_battery` is set) and in full-screen or presentation state; Triggers and user-started jobs are exempt. Also implement worker idle-exit after 60 s and the per-role worker memory budgets. Verify with simulated power-status and presentation-state tests, a worker-lifetime test and an over-budget worker test
- [x] 3.6 Implement rotating diagnostic logs with secret redaction. Verify with a log-scanning test that no token-shaped strings are written
- [x] 3.7 Implement Third-Party Notices: app.version returns every shipped component and model (core and speech-to-text package) with version, licence and required attribution, kept in step with `client/third_party/README.md`, and About lists them by package. Verify with a hidden-window test that the Parakeet CC-BY-4.0 attribution is shown under the speech-to-text package

## 4. Local storage (client/local-storage)

- [x] 4.1 Create the data root with an owner-only ACL and policy override support. Verify the ACL with an automated check
- [ ] 4.2 Implement encrypted SQLite databases with DPAPI-wrapped keys. Verify a copied database cannot be opened under another account
- [x] 4.3 Implement the migration runner with backup and rollback. Verify by injecting a failing migration and checking restore and read-only mode
- [x] 4.4 Implement the forget-cascade registry and "delete everything". Verify that registered handlers run and that the data root returns to the first-run state
- [x] 4.5 Implement the storage quota with usage breakdown. Verify the warning and indexing pause at 90% in a test

## 5. Identity and control channel (client/identity, client/control-channel, control-plane/identity, control-plane/fleet-registry)

- [ ] 5.1 Implement control-plane identity-provider trust (issuer, audience, authorized party, 10-minute lifetime cap, key reload), Groups from role mappings, direct assignment and `everyone`, admin roles with bootstrap admins, deny list and audit. Verify with tests using locally signed tokens (foreign issuer, wrong audience, foreign client, long lifetime) and with a real token from the organization Keycloak
- [x] 5.3 Implement client sign-in with PKCE, state and nonce, the loopback redirect listener open only while a sign-in is pending, ID-token verification (aud contains the client id, azp) and subject binding, the token manager (freshness check before every call, single-flight refresh, 401 retry, rotation stored encrypted, invalid_grant handling), sign-out with revocation and the sign-in required state. Verify with unit tests and end-to-end tests against the organization Keycloak, including a 5-minute token kept fresh over 20 minutes
- [x] 5.4 Implement on the client and the fleet registry:
  - control-plane discovery from machine configuration and the registration response;
  - registration and heartbeat;
  - WebSocket push with ack/replay and the registered message-type list;
  - client messages with offline queueing, routed to dispatch;
  - the connected/disconnected state rule;
  - auth-renew over the open connection and server-side closing of connections with expired tokens;
  - revocation and the minimum-version gate, at registration and through minimum-version push;
  - the user's Groups in the installation record.
  Verify end to end with a test that pushes policy-changed and receives the ack within 5 seconds, a 30-second blip test that never enters the disconnected state, and a minimum-version push test
- [x] 5.5 Load-test the fleet registry with 30k simulated WebSocket clients. Verify p99 push latency < 5 s and document the node sizing in design.md
- [x] 5.6 Implement the production issuer guard. Verify with a startup test that production refuses an http issuer

## 6. Integration

- [ ] 6.1 End-to-end test on an organization laptop against the organization Keycloak: logon, then tray, then sign-in, then registration, then appears online in the fleet registry, then refresh over an hour, then revocation locks the client
- [x] 6.2 Run `openspec validate --all --strict` and fix any findings

## Workflow follow-up

- Archive this change with `openspec archive add-client-foundation --yes` after review, then check that the merged specs read coherently.
