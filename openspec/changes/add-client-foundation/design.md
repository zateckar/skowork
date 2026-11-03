# Design

## Context

This is a greenfield project, and this change creates the repository skeleton and the system architecture that every later change builds on. The hard constraints and technology decisions are in `openspec/config.yaml` (context). This document records the rationale behind them.

The target system:

```
┌──────────────────────── Windows laptop (one per user session) ───────────────────────┐
│ Workbench.exe  (C++20/Win32, tray, per-user, non-elevated)                           │
│   host ─ identity ─ control-channel ─ local-storage      (this change)               │
│   agent-runtime ─ model-access ─ policy-enforcement      (changes 2-3)               │
│   tools: core / browser / computer / mail / knowledge / mcp / skills / tasks         │
│   UI: WebView2 window (on demand) ⇄ host via WebView2 messaging                      │
│   Local API: named pipe, current-user ACL                                            │
│ workbench-worker.exe  (role = stt | index | extract | sandbox-exec), Job Objects     │
│ Chrome (dedicated profile, CDP over pipe) · stdio MCP servers (sandboxed)            │
│ Data root: %LOCALAPPDATA%\Workbench  (encrypted SQLite DBs, DPAPI-protected keys)    │
└──────────────┬───────────────────────────────────────────────────────────────────────┘
               │ HTTPS + WSS (TLS, internal CA); Keycloak OAuth → JWT
┌──────────────▼──────────────── Control plane (Node.js + TypeScript, Express, SQLite) ─┐
│ identity · fleet-registry           (this change)                                    │
│ llm-gateway · policy-service · mcp-catalog · skill-registry · task-dispatch          │
│ telemetry-ingest · admin-console (React, served by the same process)                 │
└──────────────────────────────────────────────────────────────────────────────────────┘
```

## Goals / Non-Goals

**Goals:**
- A buildable, signed C++ client skeleton and a Node.js control-plane skeleton, with CI, that meet the footprint budgets from day one.
- End-to-end flow: Keycloak sign-in with PKCE, token refresh, device registration, heartbeat and push.
- Storage primitives that later changes reuse: encrypted databases, migrations and forget cascade hooks.

**Non-Goals:**
- Installer and updater (handled by the distribution system). This change only produces signed binaries and a portable layout.
- Any agent feature. The UI is a shell with About, Status and Settings placeholders.

## Decisions

### D1. Build a new client rather than fork an existing project
We evaluated three projects:
- IonClaw (C++20, MIT)
- pizza-bot (TypeScript/Electron, Apache-2.0)
- Hermes Agent (Python, MIT)

All three are single-user and single-tenant. None has central management, enterprise single sign-on, per-group restrictions, a knowledge graph, Outlook/Graph integration or meeting capture, and those make up most of the scope. Pizza-bot and Hermes also break the footprint budget (Electron, Python, Node and Chromium runtimes). IonClaw is light but has a single maintainer.

We reuse formats and designs rather than code:
- SKILL.md and .mcp.json formats;
- IonClaw's hook model and queue modes;
- pizza-bot's inbox UX and trigger state machine;
- Hermes's CDP browser and UI Automation approach.

Small, well-scoped pieces of MIT/Apache code may be borrowed with attribution after a licence review.

### D2. C++20/Win32 instead of .NET Native AOT, Rust or Electron
- **The platform APIs we need are native-first:** WebView2, WASAPI, UI Automation, DPAPI, Job Objects, AppContainer and Windows.Graphics.Capture.
- **The heavy libraries are C/C++:** ONNX Runtime, sherpa-onnx and SQLite.
- **.NET AOT would not save the COM work.** The WebView2 and UI Automation wrappers depend on built-in COM interop, which AOT does not support, so we would hand-write COM bindings in C# anyway.
- **C++ gives the smallest footprint:** an expected binary of about 10 MB and idle RAM of about 30–40 MB.
- **Alternatives considered:**
  - .NET 10 AOT: memory-safe and fast to develop, but has the COM issues above and a binary of about 25–40 MB.
  - Rust with windows-rs: memory-safe, but the team has less experience with it, and ML/COM bindings are less mature.
  - Electron or Tauri: Electron is too heavy. Tauri still needs native Windows work for every integration.
- **Memory-safety risk mitigation:** see Risks.

### D2a. Toolchain and dependencies: GCC on MinGW-w64, vendored sources, no package manager
- **Decision (user, 2026-10-21):** the client is built with GCC on MinGW-w64, the WinLibs distribution with the UCRT runtime, POSIX threads and SEH. The C++ runtime, libgcc and winpthread are linked statically (`-static`), so the binaries depend only on Windows system DLLs. The build uses CMake with Ninja.
- **Dependencies:** each one is vendored as pinned source under `client/third_party/<name>/`, with its version, upstream URL, SHA-256 and licence recorded in `client/third_party/README.md`, and is built by CMake. There is no package manager and no network access during the build. The set is small:
  - SQLite3MultipleCiphers (MIT), see D6;
  - sqlite-vec (MIT/Apache-2.0), added by add-knowledge-graph;
  - the WebView2 SDK header (`WebView2.h`, BSD-style);
  - a JSON parser, chosen in add-agent-runtime.
- **WebView2 loader:** the SDK's static loader library is MSVC-only, so the client ships `WebView2Loader.dll` (about 150 KB, redistributable) next to the executable and loads it with `LoadLibrary`.
- **Win32 and COM:** WIL and C++/WinRT require MSVC or clang. The client uses a small in-house header of RAII wrappers (handles, COM pointers, `CoTaskMem`). WinRT APIs (Windows.Graphics.Capture, the account broker) are called through the MinGW-w64 ABI headers. Missing declarations, such as process-loopback audio activation or `WebAuthenticationCoreManager`, are declared once in `client/src/win/abi/` from the Windows SDK documentation.
- **Alternatives considered:**
  - MSVC with vcpkg, WIL and C++/WinRT: first-class Windows tooling (Control Flow Guard, AddressSanitizer, libFuzzer). Replaced by the user's decision.
  - vcpkg with the community MinGW triplets: many ports assume MSVC or need MSYS2 tooling, and every build downloads sources. That is overkill for four dependencies.
  - llvm-mingw (clang): adds ASan, libFuzzer and Control Flow Guard on Windows, but it is a different compiler from the one decided. It stays a possible later addition for CI only.
- **Spike 1.5** confirms that the toolchain covers the platform APIs the roadmap needs.

### D3. Process model
- **Workbench.exe** is a single process holding the host, runtime, tools, storage and network. Most work is I/O-bound and waits on the LLM, so one process with a thread pool keeps idle RAM minimal.
- **`workbench-worker.exe`** runs CPU-heavy or risky work in a separate process. Its roles are `stt` (speech-to-text, the only local ML), `index` (knowledge chunking and index writes; embeddings and reranking come from the gateway), `extract` (document text extraction) and `sandbox-exec` (sandboxed execution). Each worker runs in a Job Object with kill-on-close and memory limits, and exits when idle. A worker crash never takes down the core.
- **Alternative considered:** one process doing everything. Rejected because ML memory would not be returned to the OS, and a crash in a native ML library would kill the agent.

### D4. UI technology
- **Superseded (2026-10-23):** the product owner replaced this decision with a native Win32 UI drawn with Direct2D and DirectWrite (add-native-ui). The text below records the original decision.
- **Choice:** a WebView2 single-page app (Svelte and TypeScript), embedded as a resource and served on the virtual origin `https://app.workbench.local` (D12). The host and UI communicate through WebView2 web messages with a typed JSON protocol.
- **Why:** WebView2 ships with Windows 11, so it adds nothing to the install. The UI process exists only while the window is open, so closing the window frees its RAM.
- **Alternatives considered:**
  - WinUI 3: needs the Windows App SDK runtime, and its tooling is heavier.
  - Win32/Direct2D UI: too costly for rich chat and inbox UX.
  - Qt: licensing cost and size.

### D5. Local API on a named pipe
- **Choice:** the pipe `\\.\pipe\Workbench-<user SID>`, with a DACL that grants only the user SID, and `PIPE_REJECT_REMOTE_CLIENTS`. The protocol is length-prefixed JSON-RPC 2.0. The single-instance hand-off uses the same pipe.
- **Why:** a localhost TCP port can be reached by other users on the same machine and by DNS-rebinding attacks from browsers. A pipe has OS-enforced access control.

### D6. Storage
- **Database engine:** SQLite with SQLite3MultipleCiphers (MIT; a single amalgamation with built-in crypto and no OpenSSL dependency), using WAL mode. The cipher scheme is `sqlcipher` version 4 (AES-256-CBC, HMAC-SHA512, PBKDF2-HMAC-SHA512), so the files can be opened with standard SQLCipher tools for support and forensics. Because each key is already a random 256-bit value, it is passed as a raw key and the PBKDF2 step is skipped.
- **Alternatives considered:** SQLCipher with OpenSSL (heavy MinGW build, about 3–4 MB larger); SQLCipher with a custom Windows CNG provider (smallest, but in-house crypto glue code to maintain).
- **Key handling:** one random 256-bit key per database. Each key is wrapped with DPAPI (CurrentUser, plus additional entropy bound to the user SID) and stored in `keys.bin`.
- **Data root ACL:** owner and SYSTEM only, with inheritance disabled. SYSTEM is the accepted exception in `openspec/config.yaml`, because profile backup needs it.
- **Databases:**
  - `app.db`: Threads, Runs, settings and approvals.
  - `kg.db`: knowledge.
  - `telemetry-buffer.db`
- **Migrations** are numbered SQL scripts embedded in the binary. Before a migration, the client takes an online backup to `*.bak`.
- **Forget cascade:** each feature registers forget handlers for the entity types it derives data from.
- **Profile migration:** domain DPAPI master-key backup lets a migrated profile decrypt its data. Phase-0 task 1.4 verifies this with the real migration tool.

### D7. Identity: Keycloak with OAuth 2.0 Authorization Code and PKCE
- **Decision (user, 2026-10-22):** replace Kerberos. Spike 1.1 showed that a Kerberos-only design depends on SPNs, keytabs, domain membership and WinHTTP's own authentication behaviour, which made it fragile. The organization Keycloak is already in place.
- **Keycloak client (verified 2026-10-22 with `dev/scripts/oidc-probe.mjs` against the client in `.env`):**
  - Issuer `https://idp.example.com/realms/standard`; public client (code redemption works with PKCE and no secret); PKCE S256 supported.
  - The only accepted redirect is `http://localhost:4200/*`, and the client uses `http://localhost:4200/auth/callback`. Other ports and `127.0.0.1` are rejected.
  - Scopes are `openid profile email`.
  - Access and ID tokens live 300 s. The refresh token lives 3600 s (`refresh_expires_in`) and rotates on every refresh.
  - `aud` is `[<client id>, "eai"]` and `azp` is the client id.
  - Claims include `sub`, `sid`, `preferred_username` (the user's account name), `email`, `name`, `roles` and `apps_with_role`. There is **no group claim**.
- **Client sign-in (RFC 8252 loopback redirect):**
  - The client creates a 64-byte verifier, an S256 challenge, a 32-byte state and a nonce. It binds listeners to 127.0.0.1 and ::1 on the redirect port (from machine configuration) with `SO_EXCLUSIVEADDRUSE`, so a port held by another process is detected before the browser opens. Both families are needed because "localhost" resolves to ::1 first on Windows. Then it opens the authorize URL with `ShellExecuteW` in the default browser.
  - The listener hands each request on the redirect path to the token manager before answering. A response for the pending attempt gets a static "you can close this tab" page and closes the listener. A response with an unknown state gets an error page and the listener keeps waiting, so a local process cannot end a sign-in by sending a forged request first. Other paths get 404. The listener closes anyway after 5 minutes (the sign-in then enters the sign-in required state) or on cancel. It is a minimal HTTP/1.1 reader on Winsock that reads the request line only.
  - The running instance matches the state, redeems the code at the token endpoint over WinHTTP and verifies the ID token (RS256 or ES256 via JWKS; iss, aud contains the client id, azp, nonce, exp).
  - The listener exists only while a sign-in is pending, which is the one exception to client/desktop-host's no-listening-socket rule.
- **Token manager:**
  - The access token is kept in memory. `ensure_fresh()` runs before every control-plane request and before the WebSocket handshake. It refreshes when less than 60 s of validity remains, under a mutex, so concurrent callers share one refresh.
  - On a 401 the client refreshes and retries once.
  - The rotated refresh token is written to `app.db` (`secrets` table, encrypted by the database key) before the new access token is used.
  - On `invalid_grant` the client deletes the refresh token and enters the sign-in required state.
  - The organization endpoints request an optional TLS client certificate. WinHTTP then fails with `ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED`, so the client answers with no certificate (`WINHTTP_NO_CLIENT_CERT_CONTEXT`) and resends once. This applies to HTTP and to the WebSocket handshake. The bearer token stays the only authentication.
  - The push connection sends `auth-renew` with each new access token (client/control-channel).
  - Because the refresh token lives one hour, a laptop that slept longer needs a new sign-in. The browser's SSO session usually makes that prompt-free.
- **Control plane:**
  - JWT validation with keys from the issuer's JWKS, cached. An unknown `kid` forces a reload, at most once per 60 s.
  - It checks `iss`, that `aud` contains a configured audience, that `azp` is in the allowed clients (defaults: the desktop client id for both), a lifetime of at most 10 minutes and 30 s of skew.
  - Groups are the union of `role_mappings` (Keycloak role → Group, many-to-many), direct `group_members` assignments and the implicit Group `everyone`.
  - Admin roles are assigned in the control plane (`admin_roles` table). `WORKBENCH_BOOTSTRAP_ADMINS` lists user names that always hold platform-admin, so a fresh installation can be administered.
- **Development and tests:** the organization Keycloak test realm from `.env`; there is no identity-provider mock (user decision, 2026-10-22). Automated tests cannot type a password, so the end-to-end suite needs one interactive sign-in (see D12, End-to-end tests).
- **Alternatives considered:**
  - Kerberos/Negotiate (the original D7): fragile, as above.
  - Private-use URI scheme redirect (`com.example.workbench:/…`): would avoid a listening socket, but the organization client only accepts `http://localhost:4200/*`.
  - Embedded WebView2 sign-in: RFC 8252 discourages embedded user agents, and it would not share the browser's SSO session.
  - Device authorization grant: needs a code to be typed, which is poor UX for a resident app.
  - Long-lived offline tokens (`offline_access`): they would outlive the SSO session and the client does not request them.
  - A Keycloak group claim: not emitted by the organization realm; Groups are administered in the control plane instead.
- **Risks:**
  - [The fixed loopback port 4200 is taken by another program, for example a local web development server] -> the client detects the bind failure before opening the browser and tells the user which port is in use (client/identity Loopback Redirect During Sign-In Only). A dedicated port can be registered with the realm owners later.
  - [Another local process races to bind port 4200 and receives the code] -> PKCE binds the code to the verifier held only by the client, and the state is checked, so an intercepted code is useless.
### D8. Control channel
- **Transport:** WebSocket over TLS (WinHTTP WebSocket API on the client, the `ws` package on the Node.js server). Messages use a JSON envelope `{id, type, target, issued_at, expires_at, payload}`, and the client acknowledges each by id. On reconnect, the client sends the last acknowledged id so the server can replay what it missed.
- **Alternatives considered:**
  - SignalR: its C++ client is heavy.
  - Server-sent events: half-duplex, and clients would need a second channel for acknowledgements.
  - Polling: higher latency and load.
- **Scale:** expected scale is up to about 30k connected clients. One Node.js process holds them all: an idle WebSocket costs a few kilobytes, and push traffic is light. The outbox lives in SQLite, in WAL mode. The control plane is a single node, which keeps it simple. Restarts are covered by client reconnect with replay from the outbox. Task 5.5 measures the limit.
  - **Alternative considered:** several nodes with a shared database (PostgreSQL `LISTEN/NOTIFY`). Rejected for now as more to operate (user decision 2026-10-22: Node, React and SQLite, kept simple); it becomes relevant only if one node cannot hold the fleet.
  - **Measured (task 5.5, 2026-10-26):** `control-plane/scripts/load-push.ts` opens 30,000 authenticated push WebSockets (one per seeded installation, from 8 loopback source addresses on explicit ports, because Windows has only about 16k ephemeral ports in total) to one control-plane process on a file database, then sends five `policy-changed` pushes to a Group holding all of them. Each client acknowledges every message, as the desktop client does. The load generator ran on the same machine (development workstation, i7-12700, Node 26).
    - All 30,000 connected (handshakes at about 1,900/s, 20 connects retried after a full listen backlog). Every push reached all 30,000, with none closed or missed.
    - Latency from the admin request to receipt: p50 1.6 s, p95 2.7 s, **p99 3.0 s**, max 3.1 s, under the 5 s goal. Per push, p99 grew from 2.4 s to 3.1 s as the outbox filled (150k messages).
    - Process working set: 322 MB with 30,000 idle connections, 560 MB after the pushes. The fan-out is single-threaded: one push to the whole fleet stores 30,000 outbox rows in one transaction and writes 30,000 frames, and the acknowledgements are 30,000 single-row updates.
    - **Sizing:** one node with 2 vCPUs and 1 GB of memory holds the 30k fleet. Headroom comes from the fan-out (batched acknowledgements, a prepared delivery statement), not from more nodes; rerun the script after changes to push delivery.

### D8a. Packaging and footprint
These are estimates for the baseline hardware, to be confirmed in spike 1.3. They map onto the budgets in client/desktop-host.

| Package | Contents | Estimated installed size | Budget |
|---|---|---|---|
| Core | `Workbench.exe` and `workbench-worker.exe`, about 10 MB; SQLite3MultipleCiphers with sqlite-vec, linked in, about 2 MB; `WebView2Loader.dll`, about 0.2 MB; PDFium, about 5 MB; embedded SPA, about 2 MB | about 19 MB | ≤ 25 MB |
| Speech-to-text runtime (optional) | ONNX Runtime CPU, about 15 MB; sherpa-onnx, about 5 MB | about 20 MB | ≤ 40 MB |
| Speech-to-text models (optional) | Parakeet-TDT-0.6B-v3 int8, about 640 MB; Whisper small int8 ONNX fallback, about 357 MB; VAD and diarization, about 27 MB | about 1.02 GB (as built, add-meeting-capture D1) | ≤ 1.2 GB |

The models budget was raised from 1.0 GB to 1.2 GB on 2026-10-31 (user decision). sherpa-onnx runs Whisper as ONNX, whose int8 Whisper small is 357 MB rather than the 190 MB of a whisper.cpp q5 file, which put the package just over 1.0 GB. The new budget keeps Whisper small for language identification and the fallback, with about 180 MB of headroom.

Speech-to-text is the only local ML. Embeddings and reranking for the knowledge graph use the on-prem models through the LLM gateway (decided 2026-10-21; add-knowledge-graph design D2), so no embedding or reranker model ships. The distribution system delivers the optional speech-to-text packages to the Groups that use meeting capture. Models are read-only and may be installed machine-wide; this is the one exception allowed by client/local-storage. Whisper medium is excluded, because it would exceed the models budget.

RAM: the core process idles at about 30–40 MB. The index worker runs no model and peaks at about 250 MB. Every worker's peak is bounded by Worker Memory Budgets in client/desktop-host: speech-to-text 2 GB, indexing 512 MB and extraction 1 GB.

- **Alternative considered:** local CPU embedding and reranker models (about 145 MB of models and about 300 MB of index-worker RAM). Rejected in favour of the gateway models, which are stronger on Czech and German and keep the laptop lighter; the cost is that semantic search needs the connection (client/knowledge-graph falls back to full-text and graph search offline).

### D9. Build and quality gates
- **Build:** CMake presets (debug, release, sanitize, fuzz) with GCC (D2a). The flags are:
  - warnings: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`;
  - hardening: `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=3`, `-D_GLIBCXX_ASSERTIONS`, `-fcf-protection=full`;
  - undefined behaviour: `-fsanitize=undefined -fsanitize-trap=all` in debug builds (trap mode needs no runtime library on MinGW);
  - linker: `-Wl,--dynamicbase,--nxcompat,--high-entropy-va`.
- **Code layout for testability:** parsers and other platform-independent code (JSON, HTTP and Graph payloads, MCP, CDP, document extractors, SKILL.md, path globbing) live in libraries that use no Win32 headers.
- **Sanitizers and fuzzing:** the MinGW GCC has no AddressSanitizer or libFuzzer runtime. The portable libraries are therefore also built on a Linux CI job with GCC: unit tests run under `-fsanitize=address,undefined`, and fuzz targets run under AFL++ in GCC-plugin mode. Fuzz targets use the `LLVMFuzzerTestOneInput` entry point, so they stay portable.
- **CI on every merge request:**
  - the Windows build and unit tests (GoogleTest);
  - the Linux sanitizer and fuzz smoke job;
  - `clang-tidy` over `compile_commands.json` and `-fanalyzer` on C sources;
  - a footprint check that fails the build if a budget in client/desktop-host is exceeded.
- **Signing:** Authenticode signing with the organization code-signing certificate.

### D10. Repository layout and wire contracts
These are the contracts that the client and the control plane are built against. Later changes extend them in their own design.md.

**Layout:**
- `client/`: CMake project. `src/base` (portable: JSON, base64url, URL, JWT parsing, time, logging redaction), `src/platform` (Win32 RAII, WinHTTP HTTP and WebSocket, DPAPI, BCrypt, registry, named pipe), `src/storage`, `src/identity`, `src/channel`, `src/host` (tray, window, local API), `src/app` (wiring, `Workbench.exe`), `src/worker` (`workbench-worker.exe`), `ui/` (Svelte SPA, built with npm and embedded as resources), `tests/` (GoogleTest, vendored).
- `control-plane/`: one Node.js (22+) TypeScript package. `src/server.ts` (Express app and `ws` push server), `src/<module>/` per capability (identity, fleet, gateway, policy and so on), `src/db.ts` (`node:sqlite` with numbered SQL migrations applied at start), `admin/` (React and Vite admin console, built into `dist/admin` and served by the same process), `test/` (`node:test` with an in-memory database and locally signed test tokens; tests that need the real Keycloak or LLM are tagged and run locally only), `Dockerfile`. Runtime dependencies stay few: express, ws and jose.
- `dev/graph-mock`: Microsoft Graph mock (add-mail-calendar), the only mock, because there is no access to a real tenant.
- `.github/workflows/`: `ci.yml` (every push and pull request) and `release.yml` (version tags `v*`).
- `evals/`: the agent eval suite (add-agent-runtime).

**Machine configuration** (client/control-channel Control-Plane Discovery, client/identity Identity Provider Configuration): read first from `HKLM\SOFTWARE\Policies\Workbench` (values `ControlPlaneUrl`, `IdpIssuer`, `IdpClientId`, `IdpRedirectUri`, `IdpScope`, `Channel`), then from `workbench.machine.json` next to the executable, which the distribution system or the zip package places there. User-writable locations are never read. Builds with `WORKBENCH_DEV_ENDPOINTS` (the debug and test presets, never release) also accept `http://` URLs whose host is a loopback address, so tests can run against the local control plane without certificates; release builds refuse them (client/identity Server Identity Verification).

**Control-plane HTTP API** (all under the base URL; JSON; bearer access token unless noted):
- `POST /api/v1/fleet/register` → `{installation_id, heartbeat_interval_s, min_version, revoked, services: {gateway, policy, skills, telemetry, tasks, mcp, push}, server_time}`. The request carries `installation_id, machine_name, version, channel, commit, os_build, hardware_class`. The user identity comes only from the access token (`preferred_username`); a `user_principal` in the body is ignored, so a client cannot register under another user's name. `min_version` is `"0.0.0"` (no minimum) when none is set for the channel.
- `POST /api/v1/fleet/heartbeat` `{installation_id, policy_version, status: idle|running|paused}` → `{heartbeat_interval_s, min_version}`.
- `GET /api/v1/fleet/push?installation_id=&last_ack=` (WebSocket, bearer token at the handshake). Server-to-client envelope `{id, type, target: {installation_id?, user?, group?}, issued_at, expires_at, payload}`; ids are per-installation monotonic integers. Client-to-server messages `{type, ref?, payload?}`: `ack {ref}`, `auth-renew {payload.access_token}`, `remote-task-claim|accept|decline|status {ref, payload}`. `ref` is the per-installation push message id. A claim or ack for an id that was never delivered to that installation is answered with the error code `NOT_DELIVERED` and recorded as a security audit event. The server answers a refused client message with `{type: "error", ref, payload: {code, reason}}` and closes the connection with status 4401 when the last presented token expired more than 30 s ago and 4403 when the installation or its user is revoked.
- Gateway: `/llm/v1/chat/completions`, `/llm/v1/models`, `/llm/v1/embeddings`, `/llm/v1/rerank`. The gateway is part of the control plane, so the control-plane audience is the gateway audience (client/model-access Gateway-Only Model Access).
- Errors use `{error: {code, message, reason?, retry_after_s?, reset_at?}}` with the client/model-access codes where they apply.

**Threads and I/O in the client:** the main thread runs the Win32 message loop (tray, window, WebView2). Network I/O uses synchronous WinHTTP on background threads (one long-lived thread for the push connection, a small fixed pool for requests), and results are marshalled to the main thread with a posted message. This is simpler to reason about than asynchronous WinHTTP callbacks and costs one stack per in-flight request, which is negligible at the client's concurrency (at most a handful of Runs).

**Alternatives considered:** one control-plane process per capability (more deployables for a fleet of 30k clients that one host serves); gRPC for the client API (needs HTTP/2 framing code on the client and a heavier dependency); a settings file in the data root for machine configuration (user-writable, so a user could redirect the client).

### D11. Control-plane operations
- **Production startup guard (control-plane/identity Production Issuer Guard):** the issuer guard refuses to start with an `http://` issuer. An identity provider that is unreachable at startup only logs a warning: signing keys are fetched on the first token that needs them (D7), so a Keycloak outage during a control-plane restart does not take the control plane down.
  - **Alternative considered:** failing the start when Keycloak is unreachable. Rejected because it couples the control plane's availability to Keycloak's at the worst moment (a joint restart), while requests would be refused anyway until keys are available.
- **Revocation internals (control-plane/fleet-registry Installation Revocation):** revoking an installation writes an installation deny-list entry. Revoking a user writes a user deny-list entry, blocks re-registration and closes every push connection of that user with close code 4403. The deny list is read from SQLite on every request; it is small and indexed.

### D12. Client host implementation
- **UI serving:** the SPA build (`client/ui/dist`) is packed by `tools/uipack` into one `ui.pak` blob and linked into `Workbench.exe` as an RCDATA resource. WebView2 requests to `https://app.workbench.local/*` are answered from that blob through `WebResourceRequested`, with a strict CSP (`script-src 'self'`, `connect-src 'none'`), so no UI file exists on disk and nothing can be swapped under the installed app. Navigation away from the origin, new windows and permission requests are refused. Web messages are accepted only from the app origin; each request `{id, method, params}` runs on the thread pool and the reply `{id, result|error}` is posted back on the main thread. Host events go out as `{event, data}`.
  - **Alternative considered:** `SetVirtualHostNameToFolderMapping` over an installed folder. Rejected because it needs the files on disk, and a user-writable copy could be altered.
- **Tray state precedence:** action-required, then paused, then disconnected, then running, then idle. What the user must act on comes first. Icons are drawn at runtime, so the package carries no image assets.
- **Workers:** `workbench-worker.exe --role <role> --idle-exit <s>` talks length-prefixed JSON over its stdin and stdout and inherits only those two handles. The Job Object sets kill-on-close and `JOB_OBJECT_LIMIT_PROCESS_MEMORY` to the role's budget. The limit caps committed private memory, which bounds the private working set. When the job reports the limit, the host terminates the worker, and the job fails with `resource_limit`. The worker keeps its own idle timer and exits after 60 s without a request. If a worker idles out between the host's check and a new request, the host restarts it and retries once.
- **Background gate:** one `BackgroundGate` holds the pause deadline, the battery-saver and presentation inputs, and `policy.ui.background_on_battery`. Producers ask it before each unit of work and block on it while held. Inputs are polled every 5 s: `GetSystemPowerStatus` (`SystemStatusFlag`) and `SHQueryUserNotificationState` (busy, D3D full screen, presentation mode). That costs nothing measurable and needs no power-notification registration.
- **Logs:** files are named `workbench-<ms>-<seq>.log`, so the file name gives the order. Rotation deletes by that order, never the file being written. Redaction runs before a line reaches the file in every mode.
- **Development hooks (debug and test presets only, `WORKBENCH_DEV_ENDPOINTS`):**
  - The command line accepts `--data-root`, `--pipe-name`, `--config` and `--no-register`, so tests run isolated instances next to a real one.
  - `dev_browser: "record"` in the machine configuration records the authorize URL instead of opening the browser.
  - `WORKBENCH_DEV_PROXY=http://127.0.0.1:3128` sends non-loopback requests through `dev/scripts/dev-proxy.mjs`, an HTTP CONNECT tunnel run by Node. Managed workstations can block outbound connections from unsigned executables, which includes local builds of `Workbench.exe` and `e2e_tests.exe` (seen 2026-10-22: `connect` fails with WSAEACCES while signed `node.exe` connects). TLS stays end to end; the tunnel only forwards bytes.
  - The pipe offers `dev.*` methods for worker, power-state and window checks.
  - Release builds compile none of this.
- **End-to-end tests:** `client/tests/e2e` runs against the organization Keycloak test realm (`OIDC_*` from the repository `.env`) and the real control plane started from the repository (`dev/scripts/dev-stack.ps1 -BootstrapAdmins <your Keycloak user name>`; everything stays inside the repository or the temp folder, nothing is installed elsewhere). They run locally only, never in CI.
  - Sign-in goes through the system browser and the loopback redirect, as for a user. A test cannot type a password, so the first sign-in of a run may need the developer at the browser; later sign-ins complete on the browser's SSO session within seconds. Tests that keep their session share one signed-in token manager; tests that end a session (sign-out, revocation, refresh-token reuse) sign in on their own.
  - The real realm offers no second user and no test controls, so: a data root bound to another user is simulated by presetting the bound subject; a session ended at the identity provider is simulated by revoking its refresh token at the revocation endpoint; tests that need a real 5-minute token to expire (401 retry, auth-renew past expiry) run only with `WORKBENCH_E2E_SLOW=1`, and need a synchronized workstation clock: the local control plane checks Keycloak's absolute `exp` with 30 s of skew, while the client times its refresh from `expires_in`. On 2026-10-22 the development workstation ran about 130 s fast (Windows Time stopped), and the auth-renew test failed because the control plane closed the connection before the refresh was due. Production control planes run with synchronized time. Multi-user and expired-token cases are covered by the control plane's own tests with locally signed tokens.
  - For network-blip tests, the control plane listens on another port behind an in-test TCP proxy that can drop all connections (`-CpPort 8081 -PublicBaseUrl http://127.0.0.1:8080`, `WORKBENCH_E2E_CP_UPSTREAM=8081`).

## Spike results

Run on 2026-10-21 on a development workstation, not the baseline laptop: Intel Core i7-12700, 32 GB RAM, Windows 11 23H2, workgroup (not domain-joined), GCC 16.2.0 (WinLibs, UCRT), WebView2 runtime 131. The harnesses are in `client/spikes/` and `control-plane/spikes/SpikeServer/`. A task is checked only when its stated verification ran on the stated environment.

**1.1 Kerberos (superseded on 2026-10-22 by D7 Keycloak OAuth; the harness was removed):** the Kerberos-only path could not be proven without a domain laptop, SPN and keytab, and needed WinHTTP workarounds. This led to the switch to Keycloak. The Keycloak spike is the new task 1.1.

**1.2 WebSocket (mechanics verified; corporate network open):**
- Verified locally over `wss://`, with these runs:
  - 90 s with abrupt server drops: 14 connections, 0 missing, 0 duplicates.
  - 120 s with drops: 16 connections, 59 of 59 messages delivered.
  - 90 s with five planned 12–13 s client outages: 27 messages replayed, 43 of 43 delivered, 0 duplicates.
- Reconnects took 1–6 ms once the server was reachable. The ~100 ms live latency comes from the spike server's 200 ms poll; production pushes as soon as a message is stored.
- Proxy resolution (`AUTOMATIC_PROXY`, WPAD/PAC) is logged; the machine here is direct.
- Open: on-site and VPN through the corporate proxy/PAC, the 407 path, and the 24-hour run.

**1.3 Footprint (indicative; baseline laptop open):**
- Install: `footprint-spike.exe` 2.58 MB plus `WebView2Loader.dll` 0.19 MB, about 2.8 MB against the 25 MB budget. The executable is statically linked; its imports are Windows system DLLs only (the UCRT through api-sets).
- Memory, working set / private:
  - after init (tray, pipe, WinHTTP session, encrypted `app.db` in WAL mode): 13.0 / 2.6 MB;
  - WebView2 window open: 19.5 / 3.5 MB in the core, plus the WebView2 processes;
  - idle 30 s after closing: 19.5 / 3.4 MB against the 60 MB budget.
- On close, the WebView2 browser process exits 247 ms later and no child processes remain.
- Pipe API: a second launch hands off and exits in 16 ms, and unknown methods get JSON-RPC −32601.
- The spike was removed with WebView2 (add-native-ui task 4.2). The baseline-laptop measurement for task 1.3 runs `client/tools/check-footprint.ps1` on the product build instead, and the window budgets are add-native-ui task 4.1.

**1.4 DPAPI (local behaviour verified; migration open):**
- Verified here:
  - create then verify works;
  - the database file has no plaintext SQLite header or table names;
  - the data directory has a protected DACL (user and SYSTEM only);
  - a tampered `keys.bin` fails DPAPI unwrap;
  - another database's key gives "file is not a database".
- Open: migration with the organization profile tool, as the same domain user, then `dpapi-probe verify` on the target machine.

**1.5 Toolchain (verified):** each check ran in `toolchain-probe` or `footprint-spike` and passed:
- WebView2 (SDK header plus `WebView2Loader.dll`);
- Windows.Graphics.Capture through the ABI headers (a 3840×2160 frame);
- UI Automation;
- WASAPI process loopback (16 kHz mono PCM);
- an AppContainer launch, with a profile-file read denied;
- WAM activation factory.

Gaps found and handled:
- `windows.foundation.h` does not compile in C++, because `IReference<BYTE>` and `IReference<boolean>` collide. The workaround is the include-guard wrapper `winrt_abi.h`, and `IReference<boolean>` must not be used.
- `audioclientactivationparams.h` is missing, as are the `CreateDirect3D11DeviceFromDXGIDevice` and `IDirect3DDxgiInterfaceAccess` declarations; they are declared in `abi_missing.h`.
- The WAM interop and statics interfaces are missing; add-mail-calendar declares them.
- MinGW `std::thread::native_handle()` is a `pthread_t`, not a Win32 `HANDLE`: Win32 thread APIs need threads created with `CreateThread` or the thread pool.
- libstdc++ file streams take `std::filesystem::path`, not `std::wstring`.

## Risks / Trade-offs

- [The MinGW-w64 WinRT ABI headers contain defects (the `IReference<boolean>` collision found in spike 1.5)] → Include them only through one wrapper header that applies the known workarounds; never use `IReference<boolean>`; cover every WinRT interface used with a test against the real API.
- [C++ memory-safety bugs while handling untrusted content] → Use hardened, well-fuzzed parsers; keep them in portable libraries that run under ASan, UBSan and AFL++ on the Linux CI job; use `std::span` and `_GLIBCXX_ASSERTIONS` bounds checks. Parse in workers where practical. Commission an external code review before GA.
- [GCC's PE output has no Control Flow Guard or Spectre mitigations, and Windows-only code gets no AddressSanitizer] → Keep Windows-only code thin and free of parsing. Apply the stack protector, fortify, CET-compatible code generation, DEP and high-entropy ASLR. Untrusted parsing runs in Job-Object workers. If the external review requires CFG, add llvm-mingw for release builds through a change.
- [Hand-written WinRT/COM declarations are wrong or drift from the Windows SDK] → Declare only what is used, in one place, and test each interface against the real API in spike 1.5 and in the owning change's tests.
- [Keycloak is unreachable off-network] → The offline mode is defined in client/control-channel. A valid refresh token survives short outages; VPN is required for LLM features, and users are told so in the UI.
- [A refresh token is stolen from the profile] → It is encrypted by the DPAPI-protected database key, rotated on every use (reuse revokes the session in Keycloak), and bound to the realm's SSO session limits. Sign-out revokes it.
- [DPAPI keys do not migrate with the profile tool] → Spike task 1.4. Fallback: the control plane escrows a key wrapped by DPAPI-NG (a protection descriptor for the user SID).
- [The WebView2 runtime is missing or old on some machines] → It is part of Windows 11. The client checks the minimum runtime version and reports it in registration.
- [30k persistent WebSockets overload the control plane] → Load test in task 5.5. The connection is idle-cheap, and the heartbeat interval is server-tunable.
- [Endpoint security on managed laptops blocks network access for unsigned executables (seen on the development workstation)] → Release builds must be Authenticode-signed with the organization certificate before deployment (the release workflow signs when the certificate is configured); confirm with the endpoint-security team that the signed client is allowed. Development builds use the dev proxy (D12).

## Migration Plan

Not applicable: there is no existing system. Rollout is controlled through the distribution system by Group.

## Open Questions

- What is the exact control-plane FQDN, and which internal CA issues its certificates? This is needed only for deployment configuration.
