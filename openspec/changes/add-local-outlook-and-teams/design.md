# Design

## Context

The motivation is in proposal.md. client/mail-calendar (add-mail-calendar) is built on one Graph HTTP layer. Its consumers are the mail and calendar tools, the delta sync into the local copy, the event feed for client/scheduler and client/knowledge-graph, event triage (improve-proactive-durable-work) and the Agenda intake (add-agenda). Graph cannot be used until the organization configures its Entra app registration (add-mail-calendar task 6.4). The user decided on 2026-11-21 to use the local classic Outlook and the local new Teams client instead, best effort, with no real Outlook or Teams on the development machine.

The user named three reference projects. They were studied, not copied:
- **mbe24/zaungast** (TypeScript, Apache-2.0) reads the new Teams cache. That cache is Chromium IndexedDB on LevelDB, under `%LOCALAPPDATA%\Packages\MSTeams_8wekyb3d8bbwe\LocalCache\Microsoft\MSTeams\EBWebView\<profile>\IndexedDB\https_teams.microsoft.com_0.indexeddb.leveldb`, where `<profile>` is usually `WV2Profile_tfw`. zaungast never writes to or locks the cache: it copies the immutable tables once, re-reads the write-ahead log each time, and decodes Snappy blocks and Blink/V8 serialised values itself. Its schema names the fields used here: conversation id, thread type (chat, channel, meeting, space), topic, message id, arrival time, sender MRI and display name, content (HTML), parent message, mentions, and profiles that map an MRI to a name and address. It reads no classic Teams data.
- **jagarlamudisrinath/outlook-mcp** and **lihaokun/outlook-mcp** (Python, pywin32) use the Outlook object model: `Outlook.Application`, then `GetNamespace("MAPI")`, then `GetDefaultFolder` (Inbox 6, Sent 5, Drafts 16, Outbox 4, Deleted 3, Junk 23, Calendar 9). Both serialise all COM calls behind one lock. Both report the same limits: classic Outlook only (new Outlook has no COM), Outlook must be running for the same user and elevation, and the object-model guard may prompt on send. For Exchange senders they read `Sender.GetExchangeUser().PrimarySmtpAddress`. Restrict date strings depend on the locale; one project formats them incorrectly (`%H` with `%p`).

## Goals / Non-Goals

**Goals:**
- Every behaviour of client/mail-calendar works unchanged on the local Outlook backend. No second implementation of the tools, sync, labels, exclusions, events, triage or Agenda intake.
- Teams chats feed knowledge, the Agenda and the daily report with the same filtering and triage as mail.
- Everything that does not need a running Outlook or a real Teams cache is unit-tested on the development machine.

**Non-Goals:**
- New Outlook, Outlook on the web, classic Teams, and writing to Teams.
- Full fidelity of rich content. Bodies are plain text, as on the Graph backend.

## Decisions

### D1. Backend selection
- **Decision:** policy.mail.backend = auto | graph | outlook (default auto). With auto, the backend is Graph when policy.mail.oauth is complete, and otherwise Outlook when classic Outlook is registered for the user (`CLSIDFromProgID(L"Outlook.Application")` succeeds). With neither, mail and calendar are unavailable, as before.
- The backend is part of the connection. Changing it is a new connection: the local copy is cleared because ids differ between the backends, and the next cycle is a backfill, so no events fire for old items.
- **Alternatives considered:**
  - Run both backends together: two copies of one mailbox, duplicate events. Rejected.
  - A user setting in place of policy: the organization must be able to force Graph once task 6.4 is done. The user still sees which backend is in use.

### D2. The Outlook backend answers the Graph calls in-process
- **Decision:** `GraphClient` gets an optional local transport. When it is set, `call()` hands the request (method, path with query, body) to it instead of WinHTTP. The Outlook backend is such a transport, `OutlookGraph`. It routes the subset of Graph v1.0 paths that client/mail-calendar uses, and answers with Graph-shaped JSON built from the Outlook object model. The paths are:
  - `/me`
  - `/me/mailFolders`, `/me/mailFolders/{id}/messages`, `/me/mailFolders/{id}/messages/delta`
  - `/me/messages` (list, `$search`, `$filter` on conversationId and receivedDateTime, create), and on `/me/messages/{id}`: get, PATCH, `/attachments`, `/attachments/{aid}`, `/createReply`, `/createReplyAll`, `/createForward`, `/send`, `/move`, DELETE
  - `/me/sendMail`
  - `/me/calendarView`, `/me/calendarView/delta`
  - `/me/events` (create), and on `/me/events/{id}`: get, PATCH, `/accept`, `/tentativelyAccept`, `/decline`
  - `/me/calendar/getSchedule`

  Any other path answers 400 `bad_request` (`/users/...` directory lookups are Graph-only).
- **Why:** the tools, sync, filters, label parsing and event feed are reused without change, so both backends behave alike by construction. The adapter is the only new surface, and it is testable without Outlook through an `OutlookSession` interface. `ComOutlookSession` implements that interface over COM; tests use a fake.
- **Delta emulation:** the first delta call lists every item in the window, with `@odata.deltaLink` = `outlook-delta:<key>:<generation>`. The adapter keeps an in-memory snapshot of (id, changeKey) per key. A later call with that link lists only the items whose changeKey changed, plus `{"id", "@removed"}` for items that are gone. An unknown generation (after a restart, or after Outlook was restarted) answers 410 `syncStateNotFound`. MailSync then does its existing full read, and that full read announces nothing it already has. Listing ids and change keys uses `Folder.GetTable` with the columns EntryID and LastModificationTime, which reads a whole folder without opening each item. Full items are opened only for new or changed rows.
- **Ids:** a message or folder id is its hex `EntryID`. An occurrence of a recurring event has the id `<EntryID>.<start as yyyymmddThhmm in local time>`, and its `seriesMasterId` is the EntryID. changeKey is LastModificationTime in UTC milliseconds.
- **Times:** `ReceivedTime`, `Start` and `End` are local time. The adapter reads `StartUTC`/`EndUTC` for appointments and converts other times with the Windows time-zone rules at that moment, so DST is correct.
- **Senders:** an SMTP address comes from `SenderEmailAddress` when `SenderEmailType` is `SMTP`, otherwise from `Sender.GetExchangeUser().PrimarySmtpAddress`, and then from the `PR_SMTP_ADDRESS` property (`0x39FE001F`). Recipients are resolved the same way.
- **Labels:** the adapter reads `PropertyAccessor.GetProperty("http://schemas.microsoft.com/mapi/string/{00020386-0000-0000-C000-000000000046}/msip_labels")`, which is the same MIP header Graph exposes. It returns that value as `singleValueExtendedProperties`, so label rules are unchanged.
- **Calendar view:** `Items.IncludeRecurrences = True`, `Sort("[Start]")`, then `Restrict("[Start] < '<end>' AND [End] > '<start>'")`. The dates are formatted with the user's locale short date and time (`GetDateFormatEx`/`GetTimeFormatEx`, LOCALE_USER_DEFAULT), because the Jet filter parses dates in that locale. The result is iterated with `GetFirst`/`GetNext` and capped at 2,000 occurrences.
- **Search:** with Instant Search on (`Store.IsInstantSearchEnabled`), DASL `ci_phrasematch` on subject and text body; otherwise `LIKE '%x%'`, which is slower but always available. A search is capped at the tool's limit.
- **Free/busy:** for the user and each attendee, `Recipient.FreeBusy(start, interval, True)` (`AddressEntry.GetFreeBusy`). The result is mapped to Graph's availabilityView digits; an unresolved attendee becomes an error entry.
- **Writes:**
  - `createReply`, `createReplyAll` and `createForward` call `Reply()`, `ReplyAll()` and `Forward()`, set the comment above the quoted text, and `Save()`.
  - PATCH sets subject, body and recipients, then `Save()`.
  - `send` calls `Send()`; `sendMail` creates a new item and sends it.
  - `move` calls `Move(folder)`; DELETE calls `Delete()`, which moves the item to Deleted Items in Outlook.
  - Events are created with `CreateItem(olAppointmentItem)` and made a meeting when they have attendees (`MeetingStatus = olMeeting`), then `Send()`. accept, tentativelyAccept and decline use `Respond(olMeetingAccepted|Tentative|Declined, True)` and send the response with the comment.
  - Graph's `transactionId` idempotency is emulated: the adapter remembers the item created for each transaction id for 24 hours.
- **Alternatives considered:**
  - A second set of tools and a second sync over COM: twice the code and two behaviours to keep alike.
  - Extended MAPI (`MAPILogonEx`): more control, but it needs the MAPI headers and stubs that MinGW lacks, and Microsoft supports it less than the object model.
  - EWS against the local Exchange: needs credentials and is being retired.

### D3. Outlook threading and availability
- **Decision:** one COM worker thread in the client process, created with `CoInitializeEx(COINIT_APARTMENTTHREADED)` and below-normal priority. All Outlook calls are queued to it and waited on with the caller's cancel token and a 60-second limit.
- **Attaching:** the worker attaches with `GetActiveObject` and does not start Outlook. If Outlook is not running, calls fail with `offline` ("Outlook is not running"). The tools then serve the local copy, as they do when Graph is unreachable, and sync retries next cycle. A setting lets the user allow Skowork to start Outlook (`CoCreateInstance`), default off.
- **Busy Outlook:** `RPC_E_CALL_REJECTED` and `RPC_E_SERVERCALL_RETRYLATER` (Outlook is showing a modal dialog) are retried with backoff from 1 to 30 seconds, 5 attempts, and then reported as `throttled`. A disconnected server (`RPC_E_DISCONNECTED`, Outlook exited) drops the reference; the next call attaches again.
- **Object-model guard:** Outlook trusts callers on a device with up-to-date antivirus and shows no prompt. Otherwise it may show "A program is trying to send an e-mail message" on send and on address reads. Spike 1.1 records what organization laptops do. A prompt never approves anything in Skowork: the mail.send approval floor applies first.
- **Alternatives considered:**
  - A separate worker process: isolation from Outlook hangs, but every item would be marshalled twice. The in-process thread with a time limit is enough, because COM calls are cross-process calls into Outlook already.

### D4. Reading the Teams cache
- **Location:** every `EBWebView\WV2Profile_*` folder under the new Teams package (`MSTeams_8wekyb3d8bbwe`) that contains `IndexedDB\https_teams.microsoft.com_0.indexeddb.leveldb`. With several profiles (several accounts), the settings section lists them and the user picks one. The default is the most recently modified.
- **Snapshot:** each cycle copies `CURRENT`, the current `MANIFEST-*`, every live `*.ldb` and every `*.log` into a private folder in the user's profile, then reads the copy. Files are opened with read, write and delete sharing, so Teams is never blocked, and the `LOCK` file is never opened. Table files are immutable, so a table already copied under the same name and size is reused; the logs are copied every time. A file that disappears while being copied restarts the snapshot once.
- **LevelDB:** the reader takes the live table set from the MANIFEST's version edits, so tables that compaction has made obsolete are not read and deleted keys do not come back. It merges the tables and the logs by sequence number; deletions win over older values. Blocks are Snappy-compressed or raw.
- **IndexedDB:** the reader decodes the IndexedDB key prefix (database id, object store id, index id), finds database and object store names in the metadata records, and reads object-store data records (index id 1). A value is a version varint followed by a Blink-serialised V8 value. The reader understands the Blink envelope (version tag, trailer offset), Chromium's Snappy-wrapped values (`FF 11 02`), and these V8 tags:
  - primitives and wrappers: strings (one-byte, two-byte, UTF-8), int32, uint32, double, bigint (skipped), true, false, null, undefined, and the hole;
  - containers: objects, dense and sparse arrays, Map, Set and Date;
  - object back-references and padding.

  A value that is kept in a blob file is skipped and counted.
- **What is read:**
  - Databases whose names contain `conversation-manager`, store `conversations`: id, thread type and topic.
  - `replychain-manager`, store `replychains`: the messages of each reply chain, with id, conversation id, parent id, arrival time, sender MRI and display name, content, content type, message type, edit and delete markers, and mentions.
  - `profiles`: MRI, display name and email.

  Field names are matched leniently (`originalArrivalTime` or `composeTime`, `imDisplayName` or `creatorName`). Records missing a required field are skipped and counted, so a Teams update degrades the result instead of failing.
- **Assumptions to confirm in spike 1.3** (the decoder follows Chromium's sources; no real cache was available while writing it):
  - Encoded IndexedDB key type bytes are the on-disk codes: 0 null, 1 string, 2 date, 3 number, 4 array, 5 min key, 6 binary.
  - Database ids in the name records are little-endian, minimal-length integers.
  - A `0xFE` Blink trailer tag is followed by 12 bytes.
  - Only logs at or above the MANIFEST's log number (and the previous log number) are read.
  - The user's MRI is the last GUID in a conversation-manager or replychain-manager database name.
  - A numeric time is in milliseconds.
- **Change detection:** a message is new or changed when its (id, version) is not in the local Teams copy. The version is the `version` field, or a hash of the content and edit time. Messages gone from the cache are not deleted locally: Teams also drops old messages from its cache. They leave only with the sync window, or when Teams marks them deleted.
- **Performance target:** a 200 MB cache (about 2 years of chats) read in under 20 seconds and 300 MB of peak memory on the baseline laptop. Each later cycle copies only the logs and new tables, in under 3 seconds, at the default interval of 10 minutes. Spike 1.3 measures it.
- **Alternatives considered:**
  - Microsoft Graph chat APIs: they need `Chat.Read` admin consent and the same app registration that is not available.
  - Embedding a LevelDB library: the vendored-source rule allows it, but it still lacks the IndexedDB and V8 decoding. The read-only subset is about 1,500 lines and has no writer.
  - Reading the cache in place: compaction can delete a table during the read.

### D5. The Teams store, tools and events
- **Storage:** a `teams_conversations` table and a `teams_messages` table (with FTS5) in app.db, encrypted like the mail copy. The sync window is policy.teams.sync_days (default 30). Plain text is derived from HTML (tags removed, entities decoded, `<at>` mentions kept as `@Name`). System messages (membership, call events) are kept only as metadata.
- **Tools:** teams.search and teams.read, both risk class read, with results labelled with provenance `teams`.
- **Events:**
  - `teams.message` for each new message from someone else, with `addressed` = true in a one-to-one chat or when it mentions the user;
  - `teams.updated`;
  - `teams.deleted`.

  The first read after enabling is a backfill and fires no events, as for mail.
- **The user's MRI:** taken from the database name suffix and the profile whose email matches the mail account.

### D6. Knowledge, triage and Agenda intake for Teams
- Source kind `teams` in client/knowledge-graph. A knowledge item is one conversation day: the messages of one conversation on one local date, as one text, so short chat lines carry their context. It is re-indexed when a message of that day changes. Salience and common-sense triage apply as for mail.
- Agenda intake: kind `teams`, one SourceItem per message, with the previous 5 messages of the chain as context in the text; `user_addressed` follows the event. A message from the user is offered only to detect the user's own commitments (author_is_user), as Agenda already does for meeting speakers.
- Event triage: there is no new triage scope. The daily report (D7) is where "worth knowing" Teams messages surface; act-on items come through the Agenda.

### D7. The daily report
- **Decision:** a DailyReport service writes one report per local working day. The time is chosen by the user (default 08:00) and the working days come from the user's settings (default Monday to Friday). If the client is not running at that time, it writes the report on the first start that day before 18:00. Missed days are not written later.
- **Content assembly is deterministic.** The client assembles a digest with stable ids:
  - today's events from the local calendar copy (withheld and private items as busy time only);
  - mail received since the previous report, excluding the user's own mail, drafts, withheld items and items in excluded folders, at most 60;
  - Teams messages since the previous report that are addressed to the user or in conversations the user wrote in during the last 14 days, at most 60;
  - open Agenda items due within 2 working days, new since the previous report, or overdue;
  - open triage suggestions.

  Each list keeps the newest items. The digest is at most 24,000 characters.
- **Writing:** the report is a top-level Unattended run in a new system-origin Thread titled "Daily report – <date>". Its message holds the digest as untrusted content with provenance `daily-report`. The run may use only read-risk Tools, enforced by the same tool hook as Agenda preparation, so it can look things up in knowledge. Its instruction asks for:
  1. a short "Today" section with the meetings;
  2. "Worth knowing": the mail and Teams items the user should be aware of, grouped by topic and leaving out newsletters and notifications, each with a Citation;
  3. "To act on": the Agenda items, with no new commitments invented.
- **Why a Run and not a fixed template:** choosing what is worth knowing, and grouping it, is a judgement. Agenda detection already covers acting; the report covers being informed. The model role is main, because the report is read daily.
- **Budget:** one Run per day. Its token use counts like any Unattended run.
- **Alternatives considered:**
  - A three-way triage outcome (act, inform, ignore) in event triage, with "inform" items collected for the report: it changes an unfinished change's data model and still needs a writing step.
  - A plain list without a model: long and unranked on a busy day.

### D8. Calendar page
- A Calendar page (`#/calendar`) in the navigation next to Agenda, drawn with the in-house toolkit. It has three modes:
  - Day and Work week: one column per day with the events as cards in time order, all-day events first and a "now" marker in today's column. The in-house toolkit lays out flows, not absolute positions, so a pixel time grid would need a custom drawn node without per-event accessibility; the columns keep every event a focusable, named button;
  - Agenda: a list of the next 14 days.

  It reads only the local calendar copy, through a `calendar.view` RPC that takes the range. It refreshes on calendar events and once a minute for the current-time line.
- **Event details** open in a side panel with these actions:
  - Join (when the event has an online meeting link): opens the link in the default browser;
  - Ask Skowork: a new Thread whose Run prepares a brief for the event, with the event as untrusted content (the composer has no prefill, so an empty Thread would not know the event);
  - Open in Outlook (Outlook backend): `Display()` of the item;
  - Open meeting Thread (when client/meeting-capture has one).
- **Not in this change:** creating or editing events in the page. The user asks Skowork, which uses calendar.create with its approval.

### D9. Sending a prepared draft
- **The draft:** preparation answers with a "Draft" section (add-agenda). The Agenda item and the preparation Thread show "Send draft…" when the item's draft_kind is reply and its source is mail or Teams.
- **The send form** is prefilled:
  - mail source: To is the sender and Cc the other recipients for reply-all; the subject is "Re: <subject>"; the reply keeps the conversation (`reply_to`);
  - Teams source: To is the Teams sender's address from the profile, and the subject is the chat topic or "Re: Teams – <sender>";
  - the body is the text under the Draft heading of the latest preparation.

  The user can edit everything.
- **Actions:**
  - **Send:** the user's press on Send, with the complete final message shown, is the explicit approval that the mail.send decision floor requires. No Run is involved, and the client calls the mail backend's send.
  - **Open in Outlook** (Outlook backend only): saves a reply draft and displays it in Outlook, where the user sends it.
  - **Copy:** for Teams replies the user prefers to post in Teams.

  After Send, the Agenda item offers "Mark done".
- **Alternatives considered:**
  - Letting preparation call mail.draft: add-agenda forbids preparation side effects, deliberately.
  - Approval through an Action item: the user is already looking at the complete message, so a second dialog adds nothing.

## Risks / Trade-offs

- [Outlook's object-model guard prompts on send or address reads on some laptops] -> Spike 1.1 checks organization laptops. Address reads fall back to `PR_SMTP_ADDRESS`, which the guard does not protect in the default configuration. A prompt the user declines is reported as `access_denied`.
- [Outlook is closed, or shows a modal dialog, for a long time] -> Calls time out after 60 seconds, the tools serve the local copy as offline, and sync retries next cycle. The settings section shows "Outlook is not running".
- [Teams changes its IndexedDB schema] -> Lenient field matching, per-record skipping with counters shown in settings, and a cache-format version check. A cycle that reads no conversations from a non-empty cache does not delete anything and reports "unrecognised Teams data".
- [Reading another application's cache is questioned in the privacy review] -> It is the user's own data in the user's own profile, read only after the user enables it, never sent anywhere except through the same model and knowledge rules as mail. policy.teams.mode lets the organization turn it off.
- [Jet date filters misread in some locales] -> Dates are formatted with the user's own locale functions. The adapter also checks each returned occurrence against the window in UTC and drops anything outside it.
- [The local Outlook mailbox differs from Graph's (ids, no immutable ids)] -> Switching backends clears the copy and backfills without events (D1). Ids are only used within one backend.
- [The daily report repeats what the user already read] -> Items read in Outlook (isRead) are listed with that flag, and the instruction ranks unread items first. The report never marks anything as read.
- [No Outlook or Teams on the development machine] -> Fakes and generated caches in unit tests. Tasks 1.1 to 1.3 must pass on an organization laptop before any pilot.

## Open Questions

- Do organization laptops show the object-model guard prompt (antivirus status reported to Windows Security Center)?
- Is the new Teams client deployed everywhere, or does classic Teams remain somewhere (out of scope)?
- Should the daily report also go out as a notification or mail to the user? It is a Thread with a notification only, for now.
