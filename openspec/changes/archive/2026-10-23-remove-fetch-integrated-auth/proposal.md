# Proposal

## Why

Organization sign-in is OAuth 2.0 through the organization identity provider (Keycloak), decided on 2026-10-22 to replace Kerberos. client/core-tools still specifies that web.fetch authenticates to intranet hosts with the user's Kerberos credentials, for hosts listed in policy.network.integrated_auth_hosts. The project context (`openspec/config.yaml`) still lists Windows integrated authentication among the client's WinHTTP features. Both contradict the decision. The feature is also a way for the agent to act with the user's Windows identity on any listed site, which nothing needs any more.

## What Changes

- **BREAKING (policy):** remove web.fetch's integrated authentication, the policy key policy.network.integrated_auth_hosts, and the client code that answers server Negotiate/NTLM challenges.
- web.fetch never sends the user's Windows credentials to a server. Answering a corporate proxy's own 407 challenge is unchanged, because that is the network path, not a site sign-in.
- Correct the WinHTTP line of the technology decisions in `openspec/config.yaml`.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/core-tools`: Web Fetch Tool gains "no Windows credentials to servers"; Intranet Authentication For Fetch is removed.

## Impact

- **Code:** `client/src/tools/web.*`, `client/src/platform/http.*`, `client/tests/tools/web_test.cpp`, `control-plane/src/policy/keys.ts` and the generated `client/tests/fixtures/policy/schema.json`.
- **Policy:** the control plane rejects a policy source that still sets network.integrated_auth_hosts ("unknown policy key"), as it does for any key it does not know. No released policy sets the key, the default was empty, and the local development database has no source that sets it.
- **Roadmap:** a correction to the archived add-core-tools-and-mcp; no change to the roadmap order.
