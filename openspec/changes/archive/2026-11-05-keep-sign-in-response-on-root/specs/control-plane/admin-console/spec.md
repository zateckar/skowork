# Spec Delta

## MODIFIED Requirements

### Requirement: Console Entry Point
The control plane SHALL serve the admin console at `/admin/`. When the console is built, a request for the root path `/` SHALL be redirected to `/admin/` with its query string unchanged, so a sign-in response sent to the root completes the sign-in. When SKOWORK_ADMIN_REDIRECT_URI is not set, the console SHALL use `<console origin>/admin/` as its sign-in redirect URI.

#### Scenario: Open the root path
- **WHEN** an admin opens `https://<control plane>/` in a browser
- **THEN** the browser is redirected to `https://<control plane>/admin/` and the console loads

#### Scenario: Sign-in response at the root
- **WHEN** the redirect URI is `https://<control plane>` and the identity provider sends the browser to `https://<control plane>/?state=s&code=c`
- **THEN** the browser is redirected to `https://<control plane>/admin/?state=s&code=c`, and the console exchanges the code for a token

#### Scenario: Default redirect URI
- **WHEN** SKOWORK_ADMIN_REDIRECT_URI is not set and an admin signs in from `https://cp.example.com/admin/`
- **THEN** the authorization request uses `https://cp.example.com/admin/` as its redirect URI
