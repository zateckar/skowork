# Design

## Context

- client/event-triage (improve-proactive-durable-work) consumes mail and calendar changes after consent and a baseline, with durable item and revision identity, revocation and a `small`-role classifier that returns relevant, not relevant or uncertain per event (design D1, D2 of that change). The classifier parser exists in `client/src/triage/` and is not wired in yet.
- Meeting summaries already ask the model for "follow-ups (who does what, by when)", but as free text in the meeting Thread (`client/src/meeting/meeting_service.cpp`).
- Chats come from control-plane/user-messaging; they are not Threads.
- Knowledge search, mail and calendar read Tools exist and are read-risk.
- `tasks.*` are scheduled jobs, hence the term Agenda item.

## Goals / Non-Goals

**Goals:** high precision (no nonsense on the Agenda), source-linked evidence, deduplication across sources, useful preparation with drafts, no new authority.

**Non-Goals:** a general to-do manager, sync with external task systems, acting on the user's behalf.

## Decisions

### D1. Detection piggybacks on existing intake

- **Mail and calendar:** the Agenda subscribes to mail sync changes itself. Items received or changed after the Agenda was enabled (the enablement time is the baseline; nothing older is scanned) are queued, at most 500 pending, oldest dropped with a visible count. Items the user sent, items in excluded folders and items client/mail-calendar does not make available are skipped before queuing. Batches of up to 10 items (each body cut to 6,000 bytes) go to the `small` role at most once per 60 seconds, no Tools, no streaming, strict JSON; each item may yield 0–3 candidates. A failed or malformed batch stays queued with capped backoff; a malformed response never yields partial candidates. Calendar events are queued with organizer, attendees, start and body.

  Event-triage (improve-proactive-durable-work) would serve the same purpose with durable identity, page-atomic intake and revocation, but it is not wired yet. Rather than block on it, the Agenda's intake is deliberately simpler: it is best-effort (an item missed while the client was closed is not recovered; the user is not told that coverage is complete), and revocation is handled by D4 when the item is next seen or deleted. When event-triage lands, its classifier response can carry the candidates instead.
- **Meetings:** when a captured meeting's transcript is ready, the transcript (speaker-labelled turns; a long one keeps its first third and its end, where follow-ups are usually agreed, within the 6,000-byte item limit) is queued like a mail item. A meeting addresses nobody, so admission requires the candidate or the transcript to name the user (D2 rule 2). Deleting the meeting withdraws what it supported. (Decided 2026-11-17 instead of structured follow-ups from the summary step: the transcript is already in hand when it is indexed, needs no change to the summary Run, and the quote check works on the transcript text.)
- **Chats:** new messages pushed to the user (direct Chats, and team-Chat messages, which are admitted only when they name the user) share the mail queue, batches and interval, with the same candidate schema. Chat intake starts at enablement time; earlier history is not scanned.

Candidate schema (strict JSON, bounded): `title` (≤ 120 chars, imperative), `action` (≤ 300 chars), `due` (ISO date or date-time with the source's time zone), `due_kind` (`explicit` | `inferred`), `responsibility` (`assigned` | `self_committed` | `requested`), `quote` (exact substring of the source, ≤ 300 chars), `confidence` (0..1), `draft_kind` (`reply` | `statement` | `conclusion` | `document` | `none`).

Alternatives: extend the event-triage classifier (preferred long term, but blocked by its open spike 1.1); an agent Run per item with Tools (rejected: cost and authority); local rules or regular expressions for dates (rejected: misses implicit commitments; date parsing is still validated locally, D2).

### D2. Admission rules (the nonsense filter)

A candidate becomes an Agenda item only if all of the following hold; otherwise it is dropped and counted in a diagnostic total:

1. `quote` is an exact substring of the source text the model received.
2. The user is responsible: addressed directly (To, not only Cc, or a direct Chat), named as owner, or the user wrote the commitment ("I'll send it by Friday"). Messages to distribution lists or more than 25 recipients qualify only when they name the user or the user's stated responsibilities.
3. An action by the user is required; information-only, FYI, newsletters, marketing and automated notifications are excluded.
4. The due date resolves to a moment after the source's own timestamp and after now. The model receives each item's local date and weekday and returns an ISO date or local date-time plus the exact phrase that states it; the client checks the phrase against the date: a weekday name (en, cs, de) must match the date's weekday within 13 days after the source, "today"/"dnes"/"heute" the source day, "tomorrow"/"zítra"/"morgen" the next day, "next week"/"příští týden"/"nächste Woche" 1–13 days ahead. Phrases with digits are taken as stated. A malformed or contradicting date drops the candidate. A date without a time means the end of that local day.
5. `confidence` ≥ 0.7 (policy.agenda.min_confidence).

A calendar event becomes an Agenda item only when it asks something of the user beyond attending (present, review, decide, bring, prepare); the due time is the event start.

The model's output is data: a candidate never grants authority, and the quote check prevents invented commitments.

Alternatives: show uncertain candidates in a "maybe" list (rejected for the first version: it is exactly the noise the user does not want; diagnostics keep the count); threshold only on confidence (rejected: models are overconfident; rules 1–4 are checked by the client).

### D3. Merging

Before adding, the client compares the candidate with open Agenda items due within ±7 days. Exact match on source item identity (same mail conversation, same meeting, same Chat) with overlapping titles merges directly. Otherwise, if any open item exists in the window, one `small`-role call decides `same` or `different` for up to 10 nearby items. A merge adds the source link and quote to the existing item and keeps the earlier due date. A newer revision of the same source that changes the date updates the item and shows a notice on it; when the user edited the date, the user's date stays and the notice says the source differs.

Alternatives: embedding similarity only (rejected: deadlines with similar words are often different work); never merge (rejected: a mail plus its meeting follow-up would appear twice).

### D4. Lifecycle

States: `open`, `done`, `dismissed`, `withdrawn` (its only source was deleted, excluded or revoked). The user can edit title and due date; user edits win over later source revisions, which are shown as a hint instead. Dismissal suppresses the same commitment from the same sources. An item past its due date remains open and is shown as overdue until the user marks it done or dismisses it. Items in `done` or `dismissed` are deleted 90 days after the change of state; withdrawn items are deleted at once together with their source-derived content, following the client/local-storage deletion cascade.

### D5. Preparation

Each Agenda item has at most one preparation Thread, created on first Prepare. Preparation is an ordinary top-level Run whose origin is the Agenda item; its Tools are limited to read-risk Tools (knowledge.search, mail and calendar read and search, files.read, Chat read), and policy.tools decisions still apply. Source content is untrusted and tagged with provenance as owned by client/agent-runtime.

The Run's instructions ask for:
- a brief: what is expected, by when, from whom, related decisions and open questions, each with Citations;
- where `draft_kind` is not `none`, a draft the user can edit (reply text, statement, conclusion or document outline), clearly labelled as a draft;
- missing information the user must supply.

Nothing is sent, drafted in the mailbox, scheduled or written. The user can turn the draft into a mail draft or send it only through an ordinary follow-up message in the Thread, under normal policy.

Automatic preparation: when the user enables it (and policy.agenda.auto_prepare allows), preparation starts as an Unattended run for new items due within 7 days, limited by policy.agenda.daily_preparations (default 10) and existing background deferral; policy.unattended rules apply, so a policy that does not allow read Tools unattended yields no automatic preparation. A preparation is refreshed at most once, one working day before the due date, and only if a linked source changed.

Alternatives: let preparation create mail drafts directly (rejected: write effect without the user's request); prepare every item immediately (rejected: cost; many items are trivial and due far ahead).

### D6. Presentation and reminders

A new Inbox view, Agenda, lists open items grouped as Overdue, Today, Tomorrow, This week, Later, sorted by due time. Each row shows title, due date (with "inferred" marking), the source kinds, and whether a preparation exists. Row actions: Prepare / Open preparation, Open source, Mark done, Dismiss, Edit date. A reminder notification is sent at a user-chosen lead time (default: 09:00 on the working day before, and 1 hour before a timed item), through the existing notification mechanism and respecting focus or presentation mode. The Agenda view is hidden when the Agenda is disabled.

### D7. Policy keys (owned by client/agenda)

- policy.agenda.mode: off | available (default available). With off, no detection and the view is hidden.
- policy.agenda.auto_prepare: boolean (default false). Whether the user may enable automatic preparation.
- policy.agenda.daily_preparations: integer 0–100 (default 10).
- policy.agenda.min_confidence: 0.5–0.95 (default 0.7).

User settings: Agenda on/off (default off), sources (mail, calendar, meetings, Chats), automatic preparation (default off), reminder lead times.

## Performance

Mail and calendar add at most one `small` call per minute while new items arrive (10 items per call, at most about 1,500 calls on a very busy day, counted against the background model budget). Chats add at most one `small` call per minute while messages arrive. Merge calls only when an open item is due nearby. Local work (quote check, date resolution, view grouping) is under 1 ms per candidate on the baseline i5. Storage: under 2 KB per Agenda item plus source links; at most 2,000 open items. Idle memory target: under 2 MB additional with the view closed.

## Risks / Trade-offs

- [The model invents or overstates commitments] -> Exact quote check, client-side responsibility, action and date rules, confidence threshold, labelled evaluation corpus with a precision target of at least 0.9 before release.
- [Missed commitments] -> Measure recall (target at least 0.7) on the corpus; Agenda complements, not replaces, the user's own mail reading; diagnostics show dropped counts.
- [Wrong date from relative phrases or time zones] -> Resolve dates locally with the source timestamp and time zone, mark inferred dates, let the user edit.
- [Preparation leaks or acts] -> Read-risk Tools only, untrusted provenance, no send or write, normal policy decisions.
- [Cost of automatic preparation] -> Separate opt-in, daily cap, due-within-7-days rule, one refresh at most.
- [Duplicate items from several sources] -> D3 merge, with source identity first and a bounded model decision second.
- [Revoked or deleted sources leave content behind] -> Withdraw and delete source-derived content per event-triage revocation and the local-storage cascade.

## Migration Plan

New encrypted tables in `app.db`, created disabled. No historical scan: enabling the Agenda shows items only from changes after enablement, plus the transcripts of meetings captured after enablement. Disabling stops detection; the user can delete all Agenda data.

## Implementation Notes (2026-11-17)

- Code: `client/src/agenda` (core: request, parser, admission, due dates, title similarity; store: migration 1900 tables `agenda_items`, `agenda_sources`, `agenda_pending`, `agenda_state`; service: settings, queue, detection, merging, lifecycle, reminders, preparation), `client/src/app/agenda_rpc.cpp` (agenda.status, .set, .list, .done, .dismiss, .reopen, .edit, .prepare, .delete_all, UI only) and `client/src/shell/views_agenda.cpp` (the view and the settings section).
- Intake adapters live in the app: mail.received (sent and draft folders are never "received"), event.created and event.updated, meeting transcripts when they are indexed, and chat-message pushes. mail.deleted, event.cancelled and meeting deletion withdraw. The backfill of a first sync is skipped.
- Identity for responsibility checks: the mailbox account and the signed-in principal as addresses; the display name is derived from the address (jana.novakova@ gives "jana novakova"), compared without accents.
- Merging by title uses word overlap (Jaccard over words of three letters or more, stop-words removed): the same source at 0.3, the same conversation at 0.4 within ±7 days; otherwise the small-role merge decision for up to 10 nearby items. A dismissed item suppresses candidates from the same source or conversation at overlap 0.4.
- Read-only preparation is enforced by a BeforeToolCall hook that denies any non-read Risk class for a preparation Run and its Child Runs, after the policy hook. The Thread's origin is `agenda`.
- Reminders are toasts whose activation opens `#/agenda` or the preparation Thread; they are held while presentation mode is on and shown at the next tick after it ends. The whole tick pauses while background work is paused.
- Not yet verified: the real-gateway spikes (1.1, 1.3), the e2e preparation (4.3) and the read-only hook in a running app (unit tests cover the service; the hook has no isolated test).
