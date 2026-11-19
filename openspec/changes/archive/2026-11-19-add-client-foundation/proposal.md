# Proposal

## Why

Every other Skowork capability needs four foundations: a resident, lightweight Windows client; private per-user storage; an identity from the organization identity provider that the control plane trusts; and a managed connection from each client to the control plane. Without these, nothing can be centrally managed or safely store user data, so they come first.

## What Changes

- Add the Skowork desktop host:
  - a tray-resident, per-user C++/Win32 process that starts at logon;
  - a WebView2 UI window opened on demand;
  - a local API restricted to the current user;
  - single-instance behaviour;
  - normative footprint budgets.
- Add encrypted per-user local storage in the user profile, with schema migrations and "forget" (data deletion) primitives.
- Add client identity: OAuth 2.0 Authorization Code with PKCE against the organization Keycloak in the system browser, 5-minute access tokens refreshed before every control-plane call, and an encrypted, rotating refresh token.
- Add the control-plane identity service. It validates Keycloak access tokens and assigns Groups (from token roles and from control-plane assignment) and admin roles.
- Development and tests use the organization Keycloak test realm and the real organization LLM API; there is no identity-provider mock.
- Add the client control channel: device registration, heartbeat, a persistent push connection, and a minimum-version gate.
- Add the control-plane fleet registry: it tracks clients, fans out push messages and enforces the minimum supported client version.

Out of scope:
- Installation and updates (handled by the existing distribution system).
- Profile backup and migration.
- Any agent, tool or UI feature beyond the shell window and tray. Those come in later changes.

## Capabilities

### New Capabilities
- `client/desktop-host`: process lifecycle, tray, UI window hosting, local API, single instance, footprint budgets
- `client/local-storage`: data location, encryption at rest, per-user isolation, schema migration, data deletion
- `client/identity`: sign-in with Authorization Code and PKCE, access-token freshness, refresh-token protection, sign-out
- `client/control-channel`: registration, heartbeat, push connection, minimum-version gate, offline behaviour
- `control-plane/identity`: identity-provider token validation, Groups and admin roles, deny list, audit
- `control-plane/fleet-registry`: client registry, liveness, push fan-out, minimum client version

### Modified Capabilities
- None.

## Impact

- **Roadmap:** change 1 of 13 (MVP). It depends on no earlier change. Every later change depends on it.
- **Forward references** to capabilities that later changes introduce:
  - client/tool-sandbox (change 3): the egress proxy is the only listening socket allowed;
  - control-plane/policy-service (change 3): the policy-changed push, and the `policy.ui.background_on_battery` and `policy.storage.*` keys, which take their defaults until change 3 delivers bundles;
  - control-plane/admin-console (change 7): administering Group assignments, role mappings and admin roles;
  - control-plane/skill-registry (change 9): skills-changed and skill-revoked push types;
  - control-plane/task-dispatch (change 10): Remote task push types and client messages;
  - client/agent-runtime (change 2): the Runs and Threads that the tray, Quit and storage refer to;
  - client/work-inbox (change 4): the Action items counted by the tray icon;
  - client/telemetry (change 7): reporting migration failures and liveness at telemetry level off;
  - client/scheduler (change 10): catch-up of Trigger firings suppressed by Pause background work.
  - Until those changes land, the related message types, claims and indicators are simply unused.
- **New code:**
  - `client/` (C++20 solution skeleton, host, storage, identity, channel)
  - `control-plane/` (Node.js and TypeScript service with Express and node:sqlite, Identity and Fleet modules)
- **Infrastructure:**
  - a Keycloak public client (PKCE S256, loopback redirect URI `http://localhost:4200/auth/callback`) with a 5-minute access-token lifespan; for development the client in .env (verified 2026-10-22);
  - TLS certificates from the internal CA;
  - a persistent volume for the control plane's SQLite database;
  - a DNS name for the control plane, which distribution delivers to clients as machine configuration.
- **Security review:** token format and lifetime, the local API access control, and the storage encryption design.
