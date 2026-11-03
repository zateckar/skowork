## MODIFIED Requirements

### Requirement: Web Fetch Tool
The client SHALL provide web.fetch, risk class read. It retrieves a URL over HTTP or HTTPS with GET and returns readable text, or a byte-limited raw body. Every request and redirect is checked by the network egress rules of client/policy-enforcement. Fetched content is untrusted, and client/agent-runtime records its URL as provenance. web.fetch MUST NOT send the user's Windows credentials to a server, whatever the server's authentication challenge.

#### Scenario: Host not permitted
- **WHEN** the agent calls web.fetch for a host outside policy.network.egress_allow
- **THEN** the call is denied without any network request being made

#### Scenario: Provenance recorded
- **WHEN** web.fetch returns content
- **THEN** the content enters the context marked as untrusted with the fetched URL as its provenance

#### Scenario: Server asks for Windows sign-in
- **WHEN** a server answers web.fetch with 401 and a Negotiate or NTLM challenge
- **THEN** no Windows credentials are sent, and the tool result reports the 401

## REMOVED Requirements

### Requirement: Intranet Authentication For Fetch
**Reason**: Organization sign-in is OAuth through the organization identity provider (decided 2026-10-22), not Kerberos. Sending the user's Windows identity to listed sites is no longer needed.
**Migration**: Remove policy.network.integrated_auth_hosts from policy sources. Sites that need sign-in are used through the agent browser (client/browser-use), where the user signs in through the identity provider.
