# Design

## Context

The motivation is in proposal.md. The user decided to use Microsoft Graph only, with a mock for development. The client is C++20/Win32 using WinHTTP. Mail and calendar data feeds three consumers:
- the agent tools;
- client/knowledge-graph (Sources mail and calendar);
- client/scheduler (Trigger kinds mail and calendar).

Decisions on each risk class come from client/policy-enforcement. Approval presentation is defined in client/work-inbox.

## Goals / Non-Goals

**Goals:**
- One Graph client module that serves the tools, sync and events, with a single throttling and paging implementation.
- A mock good enough that all mail and calendar tests, the CI pipeline and demos never touch a real mailbox.

**Non-Goals:**
- Full Graph coverage in the mock. It covers only the subset that the client uses.
- Shared mailboxes, delegate access, Teams chat, and offline sending through an outbox.

## Decisions

### D1. OAuth 2.0 through the organization's Entra app registration
- **Decision (user, 2026-10-21):** use the existing organization Entra app registration with OAuth 2.0 delegated permissions. Use the authorization code flow with PKCE as a public client. Mock it for now: all development and CI run against dev/graph-mock, and real-tenant integration is a later task (6.4).
- **Configuration:** policy.mail.oauth.{authority, tenant_id, client_id}. While it is empty, mail and calendar are unavailable.
- **Token acquisition, in order:**
  1. **Silent, through the Windows account broker (WAM).** `WebAuthenticationCoreManager.GetTokenSilentlyAsync` is called through the WinRT ABI, with the interface declarations the MinGW-w64 headers lack written by hand (add-client-foundation design D2a). This gives SSO with no prompt on Entra-joined or hybrid-joined devices, honours conditional access, and keeps refresh tokens inside the broker.
  2. **Interactive, in the system browser with a loopback redirect** (add-native-ui D11). The client opens the authorize URL with a PKCE challenge in the default browser and receives the code on `http://localhost:<port>/`, listening on the loopback interface only while the sign-in is pending, as client/identity does for Keycloak. Entra accepts any port on a localhost redirect URI for public clients, so the port is chosen per sign-in. The client then redeems the code at the token endpoint over WinHTTP. A refresh token issued this way is stored encrypted, per client/local-storage. The broker's own interactive UI (`IWebAuthenticationCoreManagerInterop`) is preferred where it is available.
- **Mock mode:** the same code path runs with the authority pointed at dev/graph-mock. Only the endpoints and the trusted issuer differ, so the OAuth logic is fully exercised in CI.
- **Implementation (2026-10-26):**
  - The broker is an interface (`TokenBroker`) that the token provider asks first. No WAM implementation is plugged in until spike 1.1 shows it works on organization devices, so today every token comes from the browser sign-in and its refresh token. Task 3.1 stays open for it.
  - The sign-in port is random in 49152–65000 (up to 20 tries); the redirect URI is `http://localhost:<port>/` with `response_mode=query`. The listener closes after the redirect or after 5 minutes.
  - Token endpoint errors: `interaction_required`, `consent_required` and `login_required` keep the refresh token and raise the "Connect mailbox" Question once per connection; `invalid_grant` and `expired_token` delete it and disconnect; an unreachable endpoint is `offline`.
  - Graph is a platform connection: requests go straight through the client's HTTP stack, never through the egress proxy, so policy.network rules do not apply. Retry delays (Retry-After, backoff from 1 s to 30 s, 5 attempts) are written to the Run's Activity log through `ToolContext::activity`.
- **Alternatives considered:**
  - MSAL C++: there is no maintained public native MSAL for C++ desktop.
  - A client-hosted WebView2 window that intercepts the redirect URI: the first choice, dropped when add-native-ui removed WebView2 from the client. RFC 8252 recommends the system browser for native apps, and the browser shares the user's Entra session. The loopback listener it needs is allowed by client/desktop-host while a sign-in is pending (delta in this change).
  - Device-code flow: needs user interaction on every new device, and is often blocked by conditional access.
  - A control-plane on-behalf-of exchange from the Skowork Keycloak token to a Graph token: it would centralise Graph tokens on the server, which increases blast radius, and needs a confidential client. Kept as the fallback if neither client-side path is allowed by conditional access.

### D2. Risk classes for mail and calendar tools
- **mail.draft is external, not write-local.** It writes to the server-side mailbox, so it syncs to other devices and colleagues with delegate access can see it.
  - Recommended default policy: allow without approval, because a draft is reversible and is not delivered.
- **mail.send** is external with a hard-coded approval. Policy can never remove that approval in interactive Runs. It can only allow Unattended sending through an explicit `policy.unattended.rules` entry for `mail.send`. The decision floor is defined in client/policy-enforcement.
- **mail.move and mail.delete** are destructive. Delete always moves the item to Deleted Items; it never hard-deletes.
- **calendar.create and calendar.respond** are external, because they send invitations or responses to other people.
- **mail.send requires to, subject and body in the call itself,** so the Approval shows exactly what will be sent; a reply (`reply_to`) is created, patched with exactly those fields and sent, which keeps the conversation. mail.send takes no attachments in this change.
- **calendar.create uses the tool call id as Graph's `transactionId`,** so a retried call does not send invitations twice.

### D3. Local sync store
- **What is stored:** synced message and event metadata and bodies, stored in `app.db` in mail and calendar tables, encrypted per client/local-storage. Bodies are converted to plain text.
- **Attachments:** they are fetched on demand and are not stored. Their text comes from the client/core-tools Document Text Extraction worker. Only client/knowledge-graph keeps extracted text, and only for indexing.
- **Sync window:** policy.mail.sync_window `{mail_days: 180, calendar_past_days: 90, calendar_future_days: 365}` by default, as specified in client/mail-calendar.
- **Why a local store:** it makes offline search possible and avoids re-downloading for knowledge indexing.
- **Alternative considered:** live Graph search only. Simpler, but there is no offline mode, the cost is higher and throttling would hit sooner.
- **Implementation (2026-10-26):**
  - One message delta per folder (`receivedDateTime ge` the window start, page size 50) and one calendarView delta over a day-aligned window; a new day means a new window and a full read of the calendar. Search uses an FTS5 table (`unicode61 remove_diacritics 2`).
  - The first sync of a folder is a backfill: its messages are announced with `backfill: true` and do not fire mail Triggers. New messages in Drafts, Sent Items and the Outbox are the user's own and are never "received".
  - A 410 from Graph clears the delta link and reads the folder or calendar in full once; items the store already has with the same change key are not announced again, and items Graph no longer returns are removed at the end of that full read.
  - Changing exclusions or label policy restarts every delta, so newly excluded items are dropped and newly included ones fetched.
  - Background sync runs every policy.mail.sync_interval_minutes (default 5) and is deferred while client/desktop-host pauses background work.

### D4. Sensitivity labels
- **How labels are read:** from the MIP metadata Exchange keeps in the `msip_labels` internet header, read for messages and events alike as the extended property `String {00020386-0000-0000-C000-000000000046} Name msip_labels` (`$expand=singleValueExtendedProperties($filter=id eq '...')`). The client takes the GUID of the label whose `MSIP_Label_<guid>_Enabled=true`; an item without the property, or without an enabled label, is unlabelled. Spike 1.2 confirms this against the test tenant; if the tenant exposes labels differently, only this parser and the mock's rendering change.
- **What happens to excluded items:** only the item id, label id and timestamps are stored, so that "withheld" placeholders can be shown.
- **Unlabelled items** follow policy.mail.unlabelled, as specified in client/mail-calendar (default: include).
- **Graph hosts in release builds:** the allowed set is the Microsoft Graph global and national-cloud hosts. It is compiled into the binary, so policy cannot widen it.

### D5. Event feed
- **How:** the delta processor emits normalised events onto the in-process event bus that client/scheduler and client/knowledge-graph subscribe to.
- **Calendar times:** events carry start and end times. client/scheduler owns timers relative to them, for example "15 minutes before" or "after the end", so this module has no "starting soon" timer.
- **Deduplication:** events are deduplicated by (item id, change key).

### D6. Graph mock implementation
- **Stack:** a small Node.js and TypeScript service (Express) in `dev/graph-mock/`, with in-memory state seeded from JSON fixture files in `dev/graph-mock/fixtures/`. It ships as a single-file executable and as a container image. Its tests run with `npm test` (node:test).
- **OData:** the mock supports only the operators the client emits: `eq`, `ge`, `le`, `and`, `contains`, `startswith`, `$orderby` on date fields, and `$search` as simple full text.
- **Control API:** under `/_mock/*`, on a separate port.
- **OAuth endpoints:** `/{tenant}/v2.0/.well-known/openid-configuration`, `/{tenant}/oauth2/v2.0/authorize` and `/{tenant}/oauth2/v2.0/token`, shaped like the Entra ones. The authorize step auto-consents for configured test users and redirects straight to the redirect URI with a code, so the client's loopback redirect path runs headlessly in tests, with an HTTP client following the redirect in place of the browser. The token endpoint verifies PKCE and supports authorization_code and refresh_token grants. Tokens are signed with a mock-only key and name the mock as issuer, and the Graph surface accepts only these tokens. OAuth faults are injected through the control API.
- **Express and jose,** as in the control plane, on Node.js's own TypeScript support. Delta and paging continuations stay in the mock and the links carry an opaque token, as Graph's do; a reset or `/_mock/delta/expire` raises a generation that makes every earlier token answer 410 `syncStateNotFound`. Fixture times are offsets from the fixture set's anchor (`+2d 10:00`, `-26h`), so the same seed, fixtures and anchor give identical data; pdf and docx attachments are generated from fixture text. Sending captures, then files the message in the sender's Sent Items and the inbox of fixture users; invitations and responses likewise update the attendees' and organiser's copies (same `iCalUId`).
- **Packaging:** `npm run build:sea` bundles the service and its fixtures with esbuild (a development dependency) into one CommonJS file and builds a Node.js single executable from it; the build also starts it and fails unless it is ready within 5 seconds. The container image runs the same sources on `node:24-alpine`. CI builds both.
- **Client tests:** the client's mail tests and the settings-section UI test start the mock with node (`--port 0 --control-port 0 --anchor <now>`) and skip when node or the mock's `node_modules` is missing; CI runs `npm ci` in `dev/graph-mock` with Node.js 26 before the client tests.
- **Contract tests:** recorded Graph responses (sanitised) are stored in `dev/graph-mock/contracts/` and compared with JSON-shape diffs.
- **Alternatives considered:**
  - Microsoft Dev Proxy: it mocks responses statically and has no stateful delta or send capture.
  - WireMock: stateful flows are awkward to build.

### D7. Mail and calendar Trigger sources
- **Conditions:** a mail Trigger takes `folder`, `from`, `subject_contains` and `has_attachments`; a calendar Trigger takes `at` (start or end), `offset_minutes` (−1440 to 1440), `subject_contains`, `organizer` and `external_attendees` (an attendee outside the user's mail domain). Text conditions compare case- and diacritic-insensitively.
- **Calendar offsets:** the scheduler side (`CalendarOffsets`) reads the stored events every 15 s and fires each calendar Trigger whose event time plus offset has passed since the last check. The firing key includes the event's time, so a rescheduled event fires again at its new time and a cancelled one does not fire. Withheld, private and cancelled events never fire.
- **Ids only in the Run:** client/scheduler adds a firing's detail to the Run's prompt as trusted text, so mail and calendar details carry only item ids, kinds and times, never subjects or senders. The Run reads the item with the mail tools, whose results are untrusted (provenance mail). This keeps mail content out of the trusted part of the prompt.
- **Availability:** the mail and calendar kinds exist while the mailbox is connected; disconnecting unregisters them, as client/scheduler specifies for an unavailable source.

## Risks / Trade-offs

- [Devices are not Entra-joined or hybrid-joined, so WAM cannot provide silent SSO] → The embedded interactive flow from D1 is used, with refresh tokens stored encrypted, so users sign in roughly once per refresh-token lifetime. Spike 1.1 records the join type.
- [Conditional-access policies block a non-browser public client] → Engage the identity team early, and check the app registration's redirect URIs and platform settings. Last resort: the control-plane on-behalf-of fallback in D1.
- [The OAuth path is only proven against the mock until real-tenant integration] → The mock follows the Entra endpoint shapes, and task 6.4 runs the same flow against the real app registration before any pilot.
- [Sensitivity-label metadata is not exposed reliably through Graph in this tenant] → Spike 1.2. Fallback: treat unreadable-label items as excluded when policy.mail.unlabelled = exclude.
- [The mock drifts from real Graph and tests pass falsely] → Contract tests against recordings, refreshed every quarter and whenever the client starts using a new endpoint.
- [Initial sync of large mailboxes is slow on the baseline laptop] → Sync newest items first, cap the window at 180 days, and throttle in a below-normal-priority background job. Expected: about 20k messages of metadata plus text bodies in under 30 minutes, at under 5% CPU on average.

## Open Questions

- Are all laptops Entra-joined or hybrid-joined? This only decides how often the silent broker path is used instead of the embedded sign-in. The spec is unaffected.
- What are the app registration's client id, tenant id and registered redirect URI? These are configuration for policy.mail.oauth, needed for task 6.4.
- What are the sensitivity-label GUIDs in the organization taxonomy? They are needed to configure policy.mail.excluded_labels.
