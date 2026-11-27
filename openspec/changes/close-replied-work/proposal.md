# Proposal

## Why

On 2026-11-27 a mail at 08:56 asked the user whether another invoice would come for an order. The user answered it in Outlook at 09:16. The Agenda still detected the request, prepared it automatically and showed a Thread to act on. The preparation itself said "the answer already exists in the conversation", but the item stayed open and the Thread stayed in the inbox.

Nothing in the client closes work because the user replied. Mail in the user's own folders (Sent Items, Drafts, Outbox) produces no event, so neither the Agenda nor event triage ever learns of a reply. Neither checks the conversation before it creates an item or a Thread. Detection batches wait up to 15 minutes, so a request answered within that window is always created as open work.

## What Changes

- The mail sync reports mail the user sent (a new message in Sent Items, outside the initial backfill) as `mail.sent`.
- An open Agenda item closes as done, with the notice "You replied", when the user sends mail in the mail conversation of its newest source after that source arrived. Its preparation Run is cancelled and its preparation Thread archived. Detection does not create an item from mail the user has already answered.
- An open or started triage action item from mail closes the same way, with the new suggestion state `replied`: its Run is cancelled, its Thread archived, and the Thread explains that the user replied. Triage does not create a Thread for mail the user has already answered.
- A later message in the conversation (a new request after the reply) is new work, as today.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `client/mail-calendar`: Sent Mail Event (added).
- `client/agenda`: Replied Items Close (added).
- `client/event-triage`: Replied Items Close (added).

## Impact

- `mail/mail_sync` (new `mail.sent` event and a reply lookup over the local copy), `agenda/service`, `triage/triage`, `app` wiring, schema migration 2900 (`triage_items.suggestion` gains `replied`), the Thread suggestion banner and the Agenda notice strings (en, cs, de).
- Teams: `TeamsService::user_replied` and the Teams event handling in `app`; Teams replies close the same way (design D5).
