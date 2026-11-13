# Proposal

## Why

The first production deployment on 2026-11-05 showed that the control plane does not explain how to set it up. The image started with only the two settings the startup guard asks for (OIDC_DISCOVERY_URL, SKOWORK_TELEMETRY_SECRET). After that, the root path answered "No such endpoint", the admin console could not sign anyone in, and nothing said why. The client's documentation did not say where its control-plane URL is set either.

## What Changes

- The root path `/` redirects to the admin console at `/admin/` when the console is built.
- At startup, the control plane logs the admin console URL, or a warning when the console build is missing. It also logs one warning for each missing setting that stops admins or clients from signing in or connecting:
  - the sign-in client (OIDC_CLIENT_ID);
  - the public base URL (SKOWORK_PUBLIC_BASE_URL);
  - the first admins (SKOWORK_BOOTSTRAP_ADMINS);
  - the credential key (SKOWORK_SECRET_KEY).
- The admin console sign-in redirect URI defaults to `<console origin>/admin/` when SKOWORK_ADMIN_REDIRECT_URI is not set.
- The client's machine-configuration errors name where the settings are read from (the registry policy key or `skowork.machine.json`).
- The repository gets a Docker Compose deployment with an environment template (`deploy/`). The README gets setup sections for the control plane, the identity provider client and the desktop clients.

## Out of scope

- A setup wizard in the admin console.
- Changing which settings the production startup guard requires.
- TLS termination: the compose file runs the control plane behind the organization's reverse proxy.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `control-plane/admin-console`: Console Entry Point and Deployment Setup Report (added).

## Impact

- **Roadmap:** 29 [Hardening]. Depends on add-client-foundation (D10/D11 machine configuration and operations) and add-telemetry-and-admin (admin console).
- **Code:**
  - control-plane/src/app.ts: the root redirect;
  - control-plane/src/main.ts and control-plane/src/setup-report.ts: the startup report;
  - control-plane/admin/src/auth.ts: the default redirect URI;
  - client/src/channel/machine_config.cpp: the error texts.
- **Tests:** control-plane/test/setup-report.test.ts; the root redirect in control-plane/test/admin.test.ts.
- **Data:** none.
