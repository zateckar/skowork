# Tasks

## 1. Control plane

- [x] 1.1 Remove the token exchange, the exchanged-token keys, the well-known key endpoint, the key rotation endpoint and `identity.mcpAudiences` (D5). Verify: the endpoints answer 404
- [x] 1.2 Catalog `auth` user-token or none, no audience, the migration of stored entries and keys (D4). Verify with tests: audience-token is refused, a migrated entry has user-token and no audience
- [x] 1.3 Discovery presents the requesting administrator's token (D2). Verify with tests against the MCP test server: the bearer token of create, update and re-discovery is the administrator's, and auth none sends none
- [x] 1.4 The admin console offers user token or none for a remote entry. Verify with the admin end-to-end test

## 2. Client

- [x] 2.1 Remote servers with user-token get the token manager's access token, refreshed once after a 401 (D1). Verify with MCP tests: the presented token is the user's, and a 401 leads to one refresh and one retry

## 3. Integration

- [x] 3.1 Update the MCP test server to check the issuer and expiry instead of the audience. Verify by running the control-plane tests and the client MCP tests
- [x] 3.2 Run `openspec validate --all --strict`
- [ ] 3.3 Run AppE2E.DeveloperSearchesJiraEditsAFileAndRunsABuild against the dev stack (needs an identity-provider sign-in, so only with the user's consent). Verify that discovery and the agent's Jira calls succeed with the Keycloak tokens
