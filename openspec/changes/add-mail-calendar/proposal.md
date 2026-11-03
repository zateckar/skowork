# Proposal

## Why

Much of employees' work lives in email and calendar. The agent has to read, search and draft mail, manage meetings, and feed this data to the knowledge graph and to Triggers. The organization decided to use Microsoft Graph only. Development and CI need a faithful Graph mock so no real mailbox is ever touched in tests.

## What Changes

- Add client access to the user's mailbox and calendar through Microsoft Graph:
  - OAuth 2.0 delegated access through the organization's existing Entra app registration (authorization code with PKCE; silent through the Windows account broker, otherwise an embedded sign-in with no listening socket);
  - least-privilege scopes;
  - a base URL and auth mode controlled by policy.
- Add the agent tools `mail.search`, `mail.read`, `mail.draft`, `mail.send`, `mail.move`, `mail.delete`, `calendar.list`, `calendar.get`, `calendar.find_free_time`, `calendar.create` and `calendar.respond`, each with a declared risk class.
- Add incremental delta sync of mail and calendar. It applies exclusions and sensitivity-label rules, feeds the Sources mail and calendar of client/knowledge-graph, and emits events for the Trigger kinds mail and calendar.
- Add robust Graph handling: throttling, paging, delta-token expiry and offline behaviour.
- Add `dev/graph-mock`. It is a local Graph v1.0 subset server with mocked Entra-shaped OAuth endpoints, deterministic fixtures, fault injection and an inspection API for tests. For now all development uses the mock; real-tenant integration is a later task.

Out of scope:
- Shared and delegated mailboxes.
- Teams chat and channel messages.
- Contacts and To Do.
- Exchange Web Services (EWS), COM and on-premise Exchange access.
- Mail rules and server-side forwarding.

## Capabilities

### New Capabilities
- `client/mail-calendar`: Graph access, mail and calendar tools, delta sync, exclusions and sensitivity labels, event feed for Triggers and knowledge
- `dev/graph-mock`: a local, deterministic Microsoft Graph v1.0 subset for development, automated tests and demos

### Modified Capabilities
- `client/desktop-host`: the mail sign-in's loopback redirect joins the listening sockets the client may open while a sign-in is pending

## Impact

- **Roadmap:** change 11 of 13 (Managed).
- **Depends on:**
  - add-client-foundation (identity, storage, desktop host)
  - add-agent-runtime (tools, model-access)
  - add-policy-and-sandbox (risk-class decisions, policy keys)
  - add-work-inbox (approval presentation, settings, system-origin Threads)
  - add-core-tools-and-mcp (Document Text Extraction for attachment text)
- **Consumed by:** add-scheduled-and-remote-tasks (Trigger kinds mail and calendar; it is earlier in the roadmap and consumes these events once this change lands), add-knowledge-graph (Sources mail and calendar) and add-meeting-capture (calendar cross-check).
- **Forward references:**
  - client/knowledge-graph (change 12)
  - client/meeting-capture (change 13), which consumes calendar events
- **Other capabilities referenced:**
  - The standalone "Connect mailbox" Action item of client/work-inbox (change 4).
  - The background deferral rules of client/desktop-host (change 1).
  - The platform-connection exemption from egress rules in client/policy-enforcement (change 3). The Graph host set is defined here.
- **New code:** the client Graph module, and `dev/graph-mock/` (a small Node.js and TypeScript service).
- **Infrastructure:** the organization's existing Entra app registration, used as a public client with admin-consented delegated scopes. Its broker redirect URI and a `http://localhost` loopback redirect URI must be registered. A non-production test tenant or test account is needed for contract recordings and task 6.4.
- **Security and privacy review:** scopes, label handling, and send approval.
