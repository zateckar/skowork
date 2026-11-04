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

Configuration is read from environment variables; see [`control-plane/src/config.ts`](control-plane/src/config.ts). The essentials are `OIDC_DISCOVERY_URL`, `OIDC_CLIENT_ID`, `WORKBENCH_SECRET_KEY`, `WORKBENCH_BOOTSTRAP_ADMINS` and `WORKBENCH_DB`.

Run it in a container:

```bash
docker build -t workbench-control-plane control-plane
docker run -p 8080:8080 -v workbench-data:/data --env-file .env workbench-control-plane
```

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
