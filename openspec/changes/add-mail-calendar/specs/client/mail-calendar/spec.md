# Spec Delta

## Purpose

Defines how the client reads, searches, drafts and sends the user's email and manages their calendar through Microsoft Graph. It also defines how mail and calendar changes are synchronised for the knowledge graph and for Triggers.

## ADDED Requirements

### Requirement: Graph As Sole Mail And Calendar Backend
The client SHALL access the user's mailbox and calendar only through the Microsoft Graph v1.0 API, at policy.mail.graph_base_url (default `https://graph.microsoft.com/v1.0`). It MUST NOT use EWS, MAPI, Outlook COM or any other mailbox protocol. Release builds MUST accept only Microsoft Graph service hosts. Any other host, including a dev/graph-mock instance, is accepted only by development builds.

#### Scenario: Default endpoint
- **WHEN** policy.mail.graph_base_url is not set
- **THEN** the client uses the public Microsoft Graph v1.0 endpoint

#### Scenario: Mock endpoint in development
- **WHEN** a development build runs with policy.mail.graph_base_url pointing to a dev/graph-mock instance
- **THEN** all mail and calendar operations are sent to the mock and no request reaches Microsoft Graph

#### Scenario: Non-Graph host in release build
- **WHEN** a release build receives a policy whose policy.mail.graph_base_url is not a Microsoft Graph service host
- **THEN** the client disables mail and calendar features and records a policy error event

### Requirement: Graph Platform Connection
Microsoft Graph SHALL be a platform connection of the client. Its host set is fixed to graph.microsoft.com, graph.microsoft.us, dod-graph.microsoft.us and microsoftgraph.chinacloudapi.cn, plus the dev/graph-mock host in development builds. Connections to this host set are exempt from policy.network egress rules, as client/policy-enforcement allows for platform connections. No other host gets that exemption.

#### Scenario: Egress allowlist is empty
- **WHEN** policy.network.egress_allow is empty and mail sync contacts graph.microsoft.com
- **THEN** the connection is made, because Graph is a platform connection

#### Scenario: Other Microsoft host
- **WHEN** a mail tool would contact a Microsoft host outside the Graph host set
- **THEN** that request is subject to policy.network egress rules like any other tool request

### Requirement: Entra OAuth Delegated Access
The client SHALL get Graph access through the organization's Entra app registration, using OAuth 2.0 delegated permissions. It uses the authorization code flow with PKCE as a public client. The authority, tenant and app come from policy.mail.oauth.{authority, tenant_id, client_id}, owned by this capability. All three default to empty, and while any is empty the mail and calendar features are unavailable.

#### Scenario: OAuth configuration missing
- **WHEN** policy.mail.oauth.client_id is empty
- **THEN** mail and calendar tools, Sources and Triggers are unavailable, and the Mail and calendar settings section shows that the organisation has not configured them

#### Scenario: Configured app registration
- **WHEN** policy.mail.oauth names the organization tenant and app registration
- **THEN** every token request uses that authority and client id, with a PKCE code challenge

### Requirement: Token Acquisition Through The Broker Or The System Browser
The client SHALL get tokens silently through the Windows account broker when the device supports it. Otherwise it signs the user in interactively in the default browser and receives the code on a loopback redirect URI, with the limits of the client/identity loopback redirect. The client MUST NOT host a browser or web view for sign-in, and MUST NOT ask for or store the user's password.

#### Scenario: Silent acquisition
- **WHEN** the device's account broker has a usable work account for the user
- **THEN** the client obtains a Graph token without showing any prompt

#### Scenario: Broker unavailable
- **WHEN** the device cannot use the account broker for the tenant
- **THEN** the client opens the authorize URL in the default browser, listens on `http://localhost` on the loopback interface only, on a port chosen for this sign-in, receives the authorization code, refuses any other path, and closes the listener

#### Scenario: Interactive sign-in abandoned
- **WHEN** the user closes the browser without finishing an interactive sign-in
- **THEN** after 5 minutes the client stops listening, has no listening socket for the sign-in, and the mailbox stays disconnected

#### Scenario: Interaction required
- **WHEN** silent acquisition reports that user interaction is required (for example, consent or a conditional-access step)
- **THEN** the client raises a "Connect mailbox" Question as a standalone Action item in a system-origin Thread, presented by client/work-inbox. It shows a sign-in prompt only after the user chooses Connect, and never during an Unattended run

### Requirement: OAuth Token Storage
The client SHALL hold Graph access tokens in memory only. A refresh token the client receives outside the account broker MUST be stored only encrypted in client/local-storage. Signing out of the mailbox, and the forget operations of client/local-storage, MUST delete stored refresh tokens.

#### Scenario: Restart with stored refresh token
- **WHEN** the client restarts after an interactive sign-in that issued a refresh token
- **THEN** it obtains a new access token from the encrypted refresh token without prompting

#### Scenario: Disconnect mailbox
- **WHEN** the user chooses "Disconnect mailbox" in the Mail and calendar settings section
- **THEN** the stored refresh token and all in-memory Graph tokens are deleted, and mail and calendar features stop until the user connects again

### Requirement: Least-Privilege Scopes
The client SHALL request only these delegated scopes, and only when the corresponding tools or Sources are enabled by policy: `User.Read`, `Mail.ReadWrite`, `Mail.Send`, `Calendars.ReadWrite` and `MailboxSettings.Read`. When policy.knowledge.directory_enrichment of client/knowledge-graph is on, it also requests `User.Read.All`, the least scope that returns a colleague's department, job title and manager; the scope needs admin consent.

#### Scenario: Send disabled by policy
- **WHEN** policy.tools.rules sets `mail.send` to deny for the user
- **THEN** the client does not request the `Mail.Send` scope

#### Scenario: Directory enrichment disabled
- **WHEN** policy.knowledge.directory_enrichment is off
- **THEN** the client does not request the `User.Read.All` scope

### Requirement: Mock Authentication Restricted To Development Builds
The client SHALL support policy.mail.auth with the values `oauth` (default) and `mock`. In `mock` mode, the client runs the same OAuth authorization code flow with PKCE, but against the authorize and token endpoints of dev/graph-mock and for a configured test user. The `mock` mode MUST be unavailable in release builds.

#### Scenario: Development sign-in against the mock
- **WHEN** a development build runs with policy.mail.auth = mock
- **THEN** the client completes the OAuth flow against dev/graph-mock and calls only the mock's Graph surface with the resulting token

#### Scenario: Mock auth in release build
- **WHEN** a release build receives a policy with policy.mail.auth = mock
- **THEN** the client disables mail and calendar features and records a policy error event

### Requirement: Mail Tools And Risk Classes
The client SHALL provide these mail tools with these risk classes:
- `read`: mail.search (query, folder, sender, date range, has-attachment) and mail.read (message, thread, attachment text)
- `external`: mail.draft (creates or updates a draft in the mailbox) and mail.send
- `destructive`: mail.move and mail.delete
Decisions for each class come from client/policy-enforcement. Attachment text comes from the Document Text Extraction of client/core-tools.

#### Scenario: Read a pdf attachment
- **WHEN** the agent calls mail.read for a pdf attachment of a message
- **THEN** the attachment text is produced by the client/core-tools document extraction in a sandboxed worker and returned as untrusted content

#### Scenario: Agent drafts a reply
- **WHEN** the agent calls mail.draft to reply to a message
- **THEN** a draft is saved in the user's Drafts folder and nothing is sent

#### Scenario: Agent deletes a message
- **WHEN** the agent calls mail.delete
- **THEN** the call is treated as risk class destructive, and the message moves to Deleted Items only if client/policy-enforcement allows it

### Requirement: Send Always Requires Approval
mail.send SHALL declare an approval decision floor under client/policy-enforcement. Every call requires explicit user approval of the final recipients, subject, body and attachments, whatever other policy allows. The only exception is an Unattended run in which policy.unattended.rules explicitly allows `mail.send`.

#### Scenario: Interactive send
- **WHEN** the agent calls mail.send during an interactive Run
- **THEN** the call is decided as approve, the Run is interrupted as defined by client/agent-runtime, and the Approval shows the complete message before anything is sent

#### Scenario: Unattended send
- **WHEN** a Trigger-started Run calls mail.send and policy.unattended.rules does not allow `mail.send`
- **THEN** the call is decided as approve, nothing is sent, and the Run waits on the resulting Approval Action item until the user resolves it

### Requirement: Calendar Tools And Risk Classes
The client SHALL provide these calendar tools with these risk classes:
- `read`: calendar.list (a time range across the user's calendars), calendar.get and calendar.find_free_time (the free/busy of the user and the named attendees)
- `external`: calendar.create (creates or updates an event; invitations are sent) and calendar.respond (accept, tentative or decline with an optional message)

#### Scenario: Find a meeting slot
- **WHEN** the agent calls calendar.find_free_time for three attendees and a 30-minute duration within next week
- **THEN** the tool returns candidate slots in which all attendees are free, in the user's time zone

#### Scenario: Create a meeting
- **WHEN** the agent calls calendar.create with attendees
- **THEN** the call is treated as risk class external, and invitations are sent only after the decision required by client/policy-enforcement

### Requirement: Sensitivity Label Exclusion
The client SHALL read the sensitivity label of each message and event. Items whose label is in policy.mail.excluded_labels (a list of label identifiers; default empty) MUST NOT be indexed, sent to any LLM or returned in tool results. The tools report only that an item was withheld because of its label.

#### Scenario: Strictly confidential message
- **WHEN** mail.search matches a message labelled with an excluded label
- **THEN** the result lists that message as "withheld (sensitivity label)" without its subject, body or sender details

### Requirement: Unlabelled Items
The client SHALL treat items without a readable sensitivity label according to policy.mail.unlabelled. With `include` (the default), they are handled like any other item. With `exclude`, they are withheld exactly like items with an excluded label.

#### Scenario: Unlabelled items excluded
- **WHEN** policy.mail.unlabelled is exclude and a message has no readable label
- **THEN** the message is withheld from indexing, LLM requests and tool results

### Requirement: User And Policy Exclusions
The client SHALL exclude from sync, indexing and tool results whatever policy.mail.exclusions lists, plus any exclusions the user adds. The key has three fields: `folders` (well-known or display names, default empty), `categories` (default empty) and `private_items` (true or false, default true). Users MUST NOT be able to remove policy exclusions.

#### Scenario: Private calendar item
- **WHEN** a calendar event is marked private and private items are excluded
- **THEN** calendar.list shows only its time as busy, without subject, body or attendees

### Requirement: Incremental Delta Sync
The client SHALL keep mail and calendar in sync incrementally using Graph delta queries, at the interval set by policy.mail.sync_interval_minutes (default 5). When the server reports that a delta token has expired, it SHALL fall back to a full resync. Sync is background work: it follows the Pause background work and battery-saver/presentation deferral rules of client/desktop-host.

#### Scenario: New mail arrives
- **WHEN** a new message arrives in a synced folder
- **THEN** the client receives it in the next delta cycle and publishes a "mail received" event

#### Scenario: Delta token expired
- **WHEN** Graph responds that the stored delta token is no longer valid
- **THEN** the client discards the token, performs a full resync of the affected folder or calendar, and does not emit duplicate events for items it already knows

### Requirement: Sync Window
The client SHALL sync only items inside policy.mail.sync_window. The key has three fields: `mail_days` (default 180), `calendar_past_days` (default 90) and `calendar_future_days` (default 365). Items outside the window are not stored locally. They are fetched live from Graph only when a tool asks for them.

#### Scenario: Old message requested
- **WHEN** the agent calls mail.read for a message older than the sync window
- **THEN** the message is fetched from Graph for that call and not added to the local store

#### Scenario: Online meeting link kept
- **WHEN** a synced event is an online meeting
- **THEN** the local calendar copy keeps its join link, which client/meeting-capture uses to match a detected meeting to the event

### Requirement: Event Feed For Triggers And Knowledge
The client SHALL publish normalised mail and calendar events to the Trigger kinds mail and calendar (client/scheduler) and to the Sources mail and calendar (client/knowledge-graph). The events are mail received, mail updated, mail deleted, event created, event updated and event cancelled. Calendar events carry their start and end times, so client/scheduler can compute time offsets. Excluded items are never published.

#### Scenario: Event rescheduled
- **WHEN** the organiser moves a synced meeting to a new start time
- **THEN** an "event updated" event with the event's identifier and its new start and end times is published once to client/scheduler

### Requirement: Throttling And Paging
The client SHALL follow Graph paging links to completion within each tool's result limit, honour Retry-After on throttling responses, and retry transient server errors with capped exponential backoff.

#### Scenario: Throttled request
- **WHEN** Graph responds with a throttling status and a Retry-After of 10 seconds
- **THEN** the client waits at least 10 seconds before retrying and reports the delay in the Run's Activity log

### Requirement: Offline Mail Behaviour
When Graph is unreachable, the client SHALL serve mail.search and mail.read from locally synced data where available, marked as possibly stale. Calls to external and destructive mail and calendar tools fail with a clear "offline" error.

#### Scenario: Search while offline
- **WHEN** the agent calls mail.search while Graph is unreachable
- **THEN** results come from the local synced copy and are labelled with the time of the last successful sync
