# Proposal

## Why

On 2026-11-05 admin sign-in failed in production. The deployment's sign-in redirect URI is the site root (`https://<control plane>`). Keycloak sent the browser back to `/?code=…&state=…`, the root redirect (Console Entry Point, improve-deployment-guidance) sent it on to `/admin/` without the query, and the console never saw the authorization code, so it never requested a token.

## What Changes

- The root redirect to `/admin/` keeps the query string, so a sign-in response that arrives at `/` reaches the console and completes the sign-in.

## Out of scope

- Changing which redirect URIs are accepted: any URI registered on the identity provider client that leads to the console keeps working.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `control-plane/admin-console`: Console Entry Point (modified).

## Impact

- **Roadmap:** 31 [Hardening]. A fix to improve-deployment-guidance.
- **Code:** control-plane/src/app.ts.
- **Tests:** control-plane/test/setup-report.test.ts.
- **Data:** none.
