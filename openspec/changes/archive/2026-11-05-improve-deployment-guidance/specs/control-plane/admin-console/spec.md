# Spec Delta

## ADDED Requirements

### Requirement: Console Entry Point
The control plane SHALL serve the admin console at `/admin/`. When the console is built, a request for the root path `/` SHALL be redirected to `/admin/`. When WORKBENCH_ADMIN_REDIRECT_URI is not set, the console SHALL use `<console origin>/admin/` as its sign-in redirect URI.

#### Scenario: Open the root path
- **WHEN** an admin opens `https://<control plane>/` in a browser
- **THEN** the browser is redirected to `https://<control plane>/admin/` and the console loads

#### Scenario: Default redirect URI
- **WHEN** WORKBENCH_ADMIN_REDIRECT_URI is not set and an admin signs in from `https://cp.example.com/admin/`
- **THEN** the authorization request uses `https://cp.example.com/admin/` as its redirect URI

### Requirement: Deployment Setup Report
At startup, the control plane SHALL log where the admin console is served, or a warning that its build is missing. It SHALL log one warning, naming the setting and its effect, for each missing setting among OIDC_CLIENT_ID, WORKBENCH_PUBLIC_BASE_URL, WORKBENCH_BOOTSTRAP_ADMINS, WORKBENCH_SECRET_KEY and an LLM upstream. A warning MUST NOT stop the control plane.

#### Scenario: Only the guarded settings
- **WHEN** the control plane starts in production with only OIDC_DISCOVERY_URL and WORKBENCH_TELEMETRY_SECRET set
- **THEN** it starts, logs the console URL, and logs warnings naming OIDC_CLIENT_ID, WORKBENCH_PUBLIC_BASE_URL, WORKBENCH_BOOTSTRAP_ADMINS, WORKBENCH_SECRET_KEY and OPENAI_BASE_URL

#### Scenario: Complete setup
- **WHEN** every checked setting is present
- **THEN** the startup log contains the console URL and no setup warnings
