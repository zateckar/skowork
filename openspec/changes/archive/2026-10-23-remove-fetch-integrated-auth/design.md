# Design

## Context

add-core-tools-and-mcp (archived 2026-10-23) gave web.fetch Windows integrated authentication. For hosts listed in policy.network.integrated_auth_hosts, WinHTTP's autologon policy was set to low and server Negotiate/NTLM challenges were answered with the logged-on user's credentials. On 2026-10-22 the user replaced Kerberos identity with Keycloak OAuth, and on 2026-10-23 decided to remove the feature from web.fetch as well.

## Goals / Non-Goals

**Goals:** web.fetch never authenticates to a server with Windows credentials; specs, project context, policy keys and code agree.

**Non-Goals:** web.fetch with OAuth tokens to organization applications (no current need; the agent browser covers signed-in sites). Corporate proxy authentication, which stays.

## Decisions

### D1. Autologon always high, server challenges never answered
- **Choice:** every WinHTTP request keeps `WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH`. A server's 401 is returned as is. A proxy's 407 is still answered with the default credentials, because the corporate proxy is the network path and not a site.
- **Loopback:** WinHTTP starts a Negotiate/NTLM handshake with a loopback server by itself, whatever the autologon policy. The existing guard that turns automatic authentication off for loopback hosts applies to every request now, not only to those without integrated authentication.
- **Alternative considered:** keep the feature as an opt-in for listed hosts. Rejected by the user: organization sign-in is OAuth, and the feature lets the agent act with the user's Windows identity.

### D2. The policy key goes away
- **Choice:** network.integrated_auth_hosts is removed from the control-plane key registry, and the client schema fixture is regenerated. The control plane then rejects a policy source that sets it as an unknown policy key. No source sets it today (the default was empty), so no data migration is needed.

## Risks / Trade-offs

- [An intranet page that only accepts Windows sign-in can no longer be fetched] → It can be used in the agent browser, where the user signs in.
