# Design

## D1 What counts as a reply

A message in a folder whose well-known name is `sentitems`, not a draft, with the same `conversation_id` as the source mail and a sent time later than the source's received time. This is the rule the evaluation export already uses for `user_replied`. Drafts and Outbox do not count: the mail has not left. Matching is by the Outlook or Graph conversation id only, never by subject.

## D2 When the check runs

1. On `mail.sent` from the sync (a new message in Sent Items, not backfill), the Agenda and triage close matching open work.
2. Before creating work, Agenda `accept` and triage `step` ask the local copy whether the user already replied after the source. This covers replies sent while the app was closed and replies sent before detection ran (the 15-minute batch wait).

The event path alone misses a reply sent before the item existed; the creation check alone misses a reply sent after. Both are needed.

`mail.sent` is a new kind in the mail event feed, so client/mail-calendar gains the requirement Sent Mail Event (delta in this change). It goes only to the Agenda and triage: the app does not pass it to knowledge Sources or scheduler Triggers, which keep their behaviour. The event carries the message's conversation id and sent time; the app handles it on its worker pool, because closing cancels Runs and archives Threads. The reply lookup is `MailStore::user_replied(conversation, after)`. The evaluation export keeps its own batch rule over all own folders, since it reads every message once anyway.

Mail skipped at Agenda detection because it was already answered counts as `replied_skipped` in the Agenda status, separate from the candidates dropped as noise.

## D3 Which item a reply closes

The Agenda closes an open item only when its newest source is mail in that conversation and the reply came after it. An item that also has a newer meeting or Chat source stays open, because the reply may not cover it. A triage item has one source, so the rule is that source.

A reply does not prove the request is fully handled ("I'll check tomorrow"). The close is visible and reversible: the Agenda item is done with the notice "You replied" and can be reopened; the triage Thread is archived with a banner that says why and can be restored. Leaving answered work open is the defect the user reported, so closing is the better default.

## D4 States

- Agenda: state `done`, notice `replied`. No schema change. The user decision log is not written, because the user did not decide in Skowork. Reopening the item clears the notice, since the user disagreed with the close.
- Triage: a new suggestion value `replied`. SQLite cannot change a CHECK, so migration 2900 rebuilds `triage_items` as 2700 did, keeping `instruction_id`. Like `no_action`, `replied` is final for later revisions of the same source (improve-triage-results D7), and withdrawal keeps it. Deleting triage data keeps a replied Thread, as it keeps a No action needed one. Mail answered before triage created a Thread is stored as `replied` without a Thread, and the daily quality count of created suggestions counts a replied item only when it had a Thread.

## D5 Teams

Added on 2026-11-27 at the user's request. The Teams service already publishes `teams.message` for the user's own messages with `mine` set, and it publishes no events for its backfill, so no new event is needed.

- A reply is a non-system message with `mine` in the same conversation, later than the source (by `at_ms`, then id). In a channel it must be in the same thread: its root (`parent_id`, else its own id) equals the source's root. In a one-to-one or group chat any later own message counts; as for mail (D3), a later message may not cover the request, and the close is visible and reversible.
- The rule lives in one place, `TeamsService::user_replied(conversation, message_id)`, used by triage and the Agenda both before creating work and when the user posts.
- Triage: on the user's own `teams.message`, open or started `teams` items of that conversation whose source the user has now answered become `replied`, with the same Run cancel and archive as mail. `step` marks an already-answered Teams message `replied` without a Thread.
- Agenda: `Service::user_replied` takes the source kind and an optional check of each candidate source, so a Teams item closes only when the Teams rule holds for its newest source. `accept` skips a Teams message the user already answered.
- The user's own message is still offered to the Agenda as before, so a commitment in it ("I'll send it on Friday") can still become an item.
