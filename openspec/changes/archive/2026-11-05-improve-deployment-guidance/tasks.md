## 1. Control plane

- [x] 1.1 Redirect `/` to `/admin/` when the console is built. Verify with the admin suite
- [x] 1.2 Startup setup report (setup-report.ts, main.ts). Verify with setup-report.test.ts and a production-mode start
- [x] 1.3 Admin console redirect URI defaults to `<origin>/admin/`

## 2. Client

- [x] 2.1 Machine-configuration errors name the registry key and `workbench.machine.json`

## 3. Deployment and documentation

- [x] 3.1 `deploy/docker-compose.yml` and `deploy/.env.example`
- [x] 3.2 README: control plane deployment, identity provider client, desktop client configuration, troubleshooting

## 4. Close

- [x] 4.1 Run `openspec validate --all --strict`
