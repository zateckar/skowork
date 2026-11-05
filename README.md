# Workbench

Workbench is an AI agent for Windows laptops. A central control plane manages identity, policy, LLM access, skills, MCP servers, scheduled tasks and telemetry for the desktop clients.

It ships as two artifacts:

- **Control plane**: a Node.js service (API, push channel, LLM gateway, admin console) with SQLite storage. It is published as a container image.
- **Desktop client**: a native Windows app written in C++20. It is published as a zip package.

## Repository layout

| Path | Contents |
| --- | --- |
| `client/` | Desktop client: CMake project, unit tests, fuzz targets |
| `control-plane/` | Control plane and admin console (TypeScript) |
| `dev/` | Developer tooling: Graph mock, MCP test server, helper scripts |
| `openspec/` | Specifications and change proposals (the source of truth) |
| `telemetry/` | Telemetry catalogue, which generates the allowlists |
| `.github/workflows/` | CI and release pipelines |

## Control plane

Requires Node.js 24 or later.

```bash
cd control-plane
npm ci
npm run check
npm test
npm run admin:build
```

For a local development server, run `npm run dev`. It reads the repository's `.env`; see `.env.example`. All settings are environment variables, listed in [`control-plane/src/config.ts`](control-plane/src/config.ts). To deploy the control plane, see the next section.

## Deploy the control plane

The control plane is one container: API, push channel, LLM gateway and admin console, with a SQLite database on a volume. [`deploy/docker-compose.yml`](deploy/docker-compose.yml) runs the published image.

### 1. Identity provider client (Keycloak)

The desktop clients and the admin console sign in through **one public OpenID Connect client** (Authorization Code with PKCE, no client secret). In the Keycloak realm, create or ask for a client, for example `workbench-desktop`, with:

| Setting | Value |
| --- | --- |
| Client authentication | Off (public client) |
| Standard flow | On; PKCE method `S256` |
| Valid redirect URIs | `http://localhost:4200/auth/callback` (desktop clients, loopback) and `https://<control plane>/admin/` (admin console) |
| Valid post logout redirect URIs | `https://<control plane>/admin/` |
| Web origins | `https://<control plane>` (the admin console calls the token endpoint from the browser) |
| Audience | Add an **Audience** mapper that puts the client ID itself into the access token's `aud`. The control plane only accepts tokens whose `aud` contains `OIDC_CLIENT_ID` (or `WORKBENCH_AUDIENCES`). |
| Access token lifespan | 10 minutes or less (`WORKBENCH_MAX_TOKEN_LIFETIME_S`, default 600) |

Admins are matched by the token's `preferred_username`.

### 2. Settings

```bash
cd deploy
cp .env.example .env
```

Fill in `.env`. [`deploy/.env.example`](deploy/.env.example) explains each value.

| Variable | Required | Purpose |
| --- | --- | --- |
| `WORKBENCH_PUBLIC_BASE_URL` | yes | The https address clients use, for example `https://workbench.example.com`. Registration returns service URLs under it. Without it they are derived from the request, which gives `http://` behind a TLS proxy, and clients refuse those. |
| `OIDC_DISCOVERY_URL` | yes, refuses to start without it | `https://<keycloak>/realms/<realm>/.well-known/openid-configuration` (must be https) |
| `OIDC_CLIENT_ID` | yes | The client from step 1. Without it every token is refused. |
| `WORKBENCH_BOOTSTRAP_ADMINS` | yes, for the first start | Comma-separated `preferred_username`s that get the platform-admin role. They can assign further admins in the console. |
| `WORKBENCH_TELEMETRY_SECRET` | yes, refuses to start without it | Any long random text, for example four or five random words |
| `WORKBENCH_SECRET_KEY` | yes, once an LLM connection exists | A passphrase of at least 16 characters, for example four or five random words. It encrypts LLM credentials. If it is missing while connections exist or `OPENAI_BASE_URL` is set, the control plane refuses to start. Keep it safe and unchanged: with a different value, stored credentials cannot be read and must be entered again. |
| `OPENAI_BASE_URL`, `OPENAI_KEY` | no | An OpenAI-compatible upstream that becomes the first LLM connection. Alternatively, add connections in the admin console. Without any, the gateway answers `MODEL_UNAVAILABLE`. |
| `WORKBENCH_ADMIN_REDIRECT_URI` | no | Admin console redirect. Default: `<console origin>/admin/`. The site root (`https://<control plane>`) also works, because `/` forwards the sign-in response to the console. It must match a valid redirect URI of the client exactly. |
| `WORKBENCH_TELEMETRY_FORWARD_URL`, `WORKBENCH_TELEMETRY_FORWARD_HEADERS` | no | Forward client telemetry to an OTLP/HTTP logs endpoint |

### 3. Start

```bash
docker compose up -d
docker compose logs control-plane
```

The log names the admin console URL and warns about each missing setting:

```text
Workbench control plane (production) listening on 0.0.0.0:8080
admin console: https://workbench.example.com/admin/
```

Check that the service is up with `curl http://localhost:8080/healthz`, which returns `{"status":"ok"}`.

### 4. Reverse proxy (TLS)

Desktop clients only connect over https. Put the container behind the organization's reverse proxy at the address in `WORKBENCH_PUBLIC_BASE_URL`, forwarding everything (`/`, `/admin`, `/api`, `/llm`) to port 8080 without a path prefix. The proxy must:

- forward WebSocket upgrades (`/api/v1/fleet/push`), and
- not buffer streamed responses (`/llm/...`, server-sent events).

### 5. First sign-in

Open `https://<control plane>/` (it redirects to `/admin/`) and sign in as one of `WORKBENCH_BOOTSTRAP_ADMINS`. Then:

1. Under **LLM connections**, add the LLM upstream if `OPENAI_BASE_URL` is not set.
2. Under **Models & quotas**, map the models.
3. Under **Policies**, publish a policy for `everyone`.

### 6. Remote MCP servers

A remote MCP server registered with the authentication **user's sign-in token** receives the Keycloak access token of the person using it, the same token the control plane gets. When an admin adds the server or runs discovery, that is the admin's token. When a desktop client calls it, it is the user's token. The control plane issues no tokens of its own. The MCP server must accept:

- tokens from the realm's issuer, verified against the realm's published keys;
- the authorized party (`azp`) of the client from step 1;
- the `aud` it checks, if it checks one. Add a second **Audience** mapper to the client from step 1 that puts the MCP server's audience into the token.

Discovery that fails with `the server answered HTTP 401` means the server refused the admin's token. Check these three points.

### Troubleshooting

| Symptom | Cause |
| --- | --- |
| Exits with `refusing to start: ...` | The named setting is missing: `OIDC_DISCOVERY_URL` (https) or `WORKBENCH_TELEMETRY_SECRET`. |
| `/` or `/admin` answers `{"error":{"code":"NOT_FOUND"...}}` | The admin console is not built. The startup log says so. Use the container image, or run `npm run admin:build` before `npm start`. |
| Keycloak shows "Invalid parameter: redirect_uri" | `https://<control plane>/admin/` is not a valid redirect URI of the client (step 1). |
| The console shows "Failed to fetch" after signing in | Web origins of the client do not include `https://<control plane>`. |
| The console shows `claim aud invalid` or `client ... not allowed` | No Audience mapper on the client, or `OIDC_CLIENT_ID` differs from the client the token was issued to. |
| The console says you have no access | Your `preferred_username` is not in `WORKBENCH_BOOTSTRAP_ADMINS` and no admin has given you a role. |
| Clients sign in but stay disconnected | `WORKBENCH_PUBLIC_BASE_URL` is missing or not https, or the proxy does not forward WebSocket upgrades. |

## Connect desktop clients

The client reads its control plane address from **machine configuration** that the organization software distribution sets (Intune, GPO, SCCM). Users cannot change it. Without it, the client shows *Workbench is not configured* together with the missing settings and where they are read from.

The client reads the following registry values first:

```text
HKEY_LOCAL_MACHINE\SOFTWARE\Policies\Workbench
  ControlPlaneUrl   REG_SZ  https://workbench.example.com                       (required)
  IdpIssuer         REG_SZ  https://keycloak.example.com/realms/example         (required)
  IdpClientId       REG_SZ  workbench-desktop                                   (default: workbench-desktop)
  IdpRedirectUri    REG_SZ  http://localhost:4200/auth/callback                 (default; must be http loopback)
  IdpScope          REG_SZ  openid profile email                                (default)
  Channel           REG_SZ  stable                                              (default)
```

For example, as an administrator:

```powershell
$k = 'HKLM:\SOFTWARE\Policies\Workbench'
New-Item $k -Force | Out-Null
Set-ItemProperty $k ControlPlaneUrl 'https://workbench.example.com'
Set-ItemProperty $k IdpIssuer 'https://keycloak.example.com/realms/example'
Set-ItemProperty $k IdpClientId 'workbench-desktop'
```

If the registry key has no `ControlPlaneUrl`, the client reads `workbench.machine.json` from the folder of `Workbench.exe` instead:

```json
{
  "control_plane_url": "https://workbench.example.com",
  "idp_issuer": "https://keycloak.example.com/realms/example",
  "idp_client_id": "workbench-desktop"
}
```

Optional keys are `idp_redirect_uri`, `idp_scope` and `channel`. Install the client in a folder users cannot write to, for example `C:\Program Files\Workbench`, so they cannot replace this file. The values must match the server: `ControlPlaneUrl` = `WORKBENCH_PUBLIC_BASE_URL`, `IdpIssuer` = the issuer of `OIDC_DISCOVERY_URL`, and `IdpClientId` = `OIDC_CLIENT_ID`. Both URLs must be https.

## Desktop client

Requires GCC on MinGW-w64 (CI uses WinLibs GCC 16.2.0), CMake 3.25 or later, and Ninja.

```powershell
cd client
cmake --preset debug
cmake --build --preset debug
ctest --preset debug -LE e2e
```

To build the package, run `cmake --preset release`, build the release preset, then run `tools/package.ps1 -BuildDir build/release -OutDir build/package -Version <version>`.

The end-to-end tests (`-L e2e`) need an OIDC test realm and an LLM API. Copy `.env.example` to `.env` and fill it in; they are not run in CI.

## Releases

Pushing a tag such as `v1.2.3` runs [`.github/workflows/release.yml`](.github/workflows/release.yml), which:

- publishes the control plane as `ghcr.io/<owner>/workbench-control-plane:1.2.3` (and `latest` for stable versions), and
- builds the desktop client and attaches it to a GitHub Release, each zip with a SHA-256 checksum file:
  - `Workbench-1.2.3-win-x64.zip`: the client;
  - `Workbench-1.2.3-app-runtime-win-x64.zip`: optional, the Node.js runtime for Local apps. Extract it next to `Workbench.exe`; without it the client works, but Local apps are not available.

Tags with a suffix, such as `v1.2.3-rc.1`, are published as pre-releases and do not update `latest`. Code signing is optional: it runs when the repository secrets `WORKBENCH_SIGNING_CERT_PFX_BASE64` and `WORKBENCH_SIGNING_CERT_PASSWORD` are set.

## Specifications

Changes are spec-driven with [OpenSpec](https://github.com/Fission-AI/OpenSpec). Read [`AGENTS.md`](AGENTS.md) and [`openspec/config.yaml`](openspec/config.yaml) before making a change.

## License

[MIT](LICENSE)
