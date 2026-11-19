# Tasks

## 1. Validation spikes

- [x] 1.1 Moved to verify-foundations-on-target 1.1 (organization laptop and Keycloak realm). Verified on the development workstation on 2026-10-22; see design.md D7.
- [x] 1.2 Moved to verify-foundations-on-target 2.1 (corporate proxy/PAC and VPN).
- [x] 1.3 Moved to verify-foundations-on-target 3.1 (baseline laptop). The WebView2 skeleton is obsolete: add-native-ui replaced it, so the measurement uses the release client.
- [x] 1.4 Moved to verify-foundations-on-target 3.2 (organization profile-migration tool).
- [x] 1.5 Prove the GCC/MinGW toolchain covers the roadmap's platform APIs: WebView2 through the SDK header and `WebView2Loader.dll`, a WinRT call through the ABI headers (Windows.Graphics.Capture), UI Automation, WASAPI process-loopback activation with hand-written declarations, and an AppContainer process launch. Verify each with a minimal program that runs on Windows 11, and record any gaps in design.md

## 2. Repository and build

- [x] 2.1 The `client/` CMake solution builds with GCC/MinGW. It has debug, test, release and fuzz presets and pinned vendored sources. Verified 2026-11-19: the release `Skowork.exe` imports only Windows system DLLs. The native UI needs no `WebView2Loader.dll`. Signing in CI moved to verify-foundations-on-target 6.1.
- [x] 2.2 Create the `control-plane/` Node.js TypeScript service (Express, node:sqlite with migrations, `ws`) with the Identity and Fleet modules, a React admin console shell served by the same process, a Dockerfile, and `dev/scripts/dev-stack.ps1`, which runs it from the repository. Verify that `npm test` passes locally and in CI
- [x] 2.3 CI quality gates follow client/build-pipeline: CI compiles the release client, checks manifests and footprint, and validates OpenSpec. Client tests and fuzzing stay local. Verified 2026-11-19: `check-footprint.ps1 -InstallOnly -MaxInstallMB 1`, the step CI runs, exits 1 with "Footprint budget exceeded". With the 30 MB budget it exits 0 at 28.89 MB.
- [x] 2.4 Add GitHub Actions release workflows that publish two artifacts for a version tag: the control plane as a container image and the desktop client as a zip package with its checksum. Verify a tag build produces both
- [x] 2.5 Moved to verify-foundations-on-target 6.2 (needs a push to `main` and a pull request). The workflow is in `.github/workflows/ci.yml`.

## 3. Desktop host (client/desktop-host)

- [x] 3.1 Implement per-user start at logon (HKCU Run key) with repair, single instance and argument hand-off over the named pipe. Remove the private-use URI scheme registration. Verify with an automated test that launches twice
- [x] 3.2 Tray icon states, menu, unread badge and confirmation on Quit are implemented. Verified 2026-11-19 by Tray.StateRules, Tray.IconReflectsEachState, Tray.AppIconWithStateDot and Tray.UnreadMessagesBadgeAndTooltip.
- [x] 3.3 Implement the on-demand WebView2 window with the embedded SPA shell (About, Status, Settings placeholders). Verify that closing the window terminates the WebView2 processes
- [x] 3.4 The named-pipe JSON-RPC API uses a user-only DACL (`D:P(A;;GA;;;<user SID>)`) and PIPE_REJECT_REMOTE_CLIENTS. Verified 2026-11-19 by Pipe.DaclGrantsOnlyTheCurrentUser, Pipe.EchoAndSingleOwner and Pipe.RefusesOversizedFrames. The different-user connection and the listening-socket check moved to verify-foundations-on-target 4.1.
- [x] 3.5 Implement Pause background work (indexing and embedding, transcription, mail/calendar sync and Trigger firings) and background deferral. Deferral covers indexing and embedding, background transcription and sync on battery saver (unless `policy.ui.background_on_battery` is set) and in full-screen or presentation state; Triggers and user-started jobs are exempt. Also implement worker idle-exit after 60 s and the per-role worker memory budgets. Verify with simulated power-status and presentation-state tests, a worker-lifetime test and an over-budget worker test
- [x] 3.6 Implement rotating diagnostic logs with secret redaction. Verify with a log-scanning test that no token-shaped strings are written
- [x] 3.7 Implement Third-Party Notices: app.version returns every shipped component and model (core and speech-to-text package) with version, licence and required attribution, kept in step with `client/third_party/README.md`, and About lists them by package. Verify with a hidden-window test that the Parakeet CC-BY-4.0 attribution is shown under the speech-to-text package

## 4. Local storage (client/local-storage)

- [x] 4.1 Create the data root with an owner-only ACL and policy override support. Verify the ACL with an automated check
- [x] 4.2 Encrypted SQLite databases use DPAPI-wrapped keys. Verified 2026-11-19 by Dpapi.RoundTripBoundToPurpose and Dpapi.RejectsGarbage. Opening a copied database under another account moved to verify-foundations-on-target 4.2.
- [x] 4.3 Implement the migration runner with backup and rollback. Verify by injecting a failing migration and checking restore and read-only mode
- [x] 4.4 Implement the forget-cascade registry and "delete everything". Verify that registered handlers run and that the data root returns to the first-run state
- [x] 4.5 Implement the storage quota with usage breakdown. Verify the warning and indexing pause at 90% in a test

## 5. Identity and control channel (client/identity, client/control-channel, control-plane/identity, control-plane/fleet-registry)

- [x] 5.1 Control-plane identity-provider trust, Groups, admin roles, deny list and audit are implemented. Verified 2026-11-19 by the 26 tests in `control-plane/test/identity.test.ts`, which use locally signed tokens (foreign key, issuer, audience and client, long lifetime, clock skew). The real Keycloak token check moved to verify-foundations-on-target 1.2.
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

- [x] 6.1 Moved to verify-foundations-on-target 7.3 (organization laptop and Keycloak).
- [x] 6.2 Run `openspec validate --all --strict` and fix any findings

## Workflow follow-up

- Archive this change with `openspec archive add-client-foundation --yes` after review, then check that the merged specs read coherently.
