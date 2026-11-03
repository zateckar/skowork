# Tasks

## 1. Remove integrated authentication

- [x] 1.1 Remove `integrated_auth` from the HTTP request and the server-challenge answer in `client/src/platform/http.*`; keep the 407 proxy answer, and keep automatic authentication off for loopback hosts on every request. Verify that a loopback server's Negotiate/NTLM challenge gets no credentials and web.fetch reports the 401
- [x] 1.2 Remove `integrated_auth_host` and its use from web.fetch, and network.integrated_auth_hosts from `control-plane/src/policy/keys.ts`; regenerate `client/tests/fixtures/policy/schema.json`. Verify with the control-plane tests and the client tests
- [x] 1.3 Correct the WinHTTP line of the technology decisions in `openspec/config.yaml`
- [x] 1.4 Run `openspec validate --all --strict` and fix any findings
