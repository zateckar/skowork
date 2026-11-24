# Design: triage results instead of suggestions

## Context
Event triage (improve-proactive-durable-work, add-teams-triage) classifies eligible items as relevant, not relevant or uncertain. It creates a system-origin suggestion Thread for each relevant item, and the user starts work by hand. The first use on a real mailbox gave a list of "Suggestion: …" Threads with a "Skowork" badge. The user asked for automatic processing: information or action required, and for an action, a draft of that action.

This change builds on two in-flight changes, so it must be archived after improve-proactive-durable-work (which adds client/event-triage) and after add-local-outlook-and-teams (which adds client/daily-report).

## Decisions

### D1. Classification gives action and category
The batch response object for a relevant item carries two more fields:
- `action`: `required` or `info`;
- `category`: one of request, approval, question, meeting, deadline, decision, update.

A missing field or an unknown value fails the batch, as any malformed result does today. Not relevant and uncertain results may leave both out. The fields are stored on triage_items (migration 2600: action, category, reported_at).

### D2. Action items are prepared automatically, read-only, in the Thread
The worker creates the Thread and marks the item `started`, then starts the Run through `start_run` with `unattended = true` and the source as untrusted content with its provenance. The preamble asks for a summary and a draft under "Draft", and forbids every write. Writes are also blocked by client/policy-enforcement for Unattended runs over untrusted content, so the preamble is not the safety boundary.

The draft is not written to Outlook. The user chose this: a draft in the mailbox would be a write caused by untrusted content without the user's review. Sending stays an explicit, approved action by the user in that Thread. D6 replaces the draft in prose with options on a Question card.

The worker does not start a preparation when:
- automatic preparation is off;
- policy.unattended.allowed_risk lacks read;
- 30 preparations already started today (counter `prepared.<local day>`).

The Thread then keeps Start work. A prepared item is `started`, and as for any started item, later revisions do not rewrite it, so a source change never starts a second Run; the user can ask for an update in the Thread.

Preparation starts after the classification transaction commits, never inside it: starting a Run touches the runtime's own tables.

### D3. Information items wait for the Daily report
They get no Thread: the user chose the digest only. They stay in triage_items with action `info`, and `reported_at` is set when a report includes them. They are dropped from the digest after 7 days.

`TriageService::digest()` returns the unreported information items and the open action items. The report's collection passes both, which also replaces the `open_suggestions` key the report read from status() and that status() never set.

### D4. Badges come from the Thread's origin ref
The Thread's origin ref already carries `{"kind":"triage", scope, item_id}`, and it now also carries `action` and `category`. The shell shows the category (neutral) instead of the system origin badge, with the status as in D8, in the list and in the Thread header. Threads created before this change have no category and show only the status.

### D5. Titles
`source_title` returns the subject (or, for Teams, the sender and the start of the message) without "Suggestion: ". The neutral title of a withdrawn item stays.

### D6. Decisions as Question card options (acceptance feedback 2026-11-24)
The first prepared Threads showed a good summary, but a draft in prose left the next step unclear. The user asked for clickable options, input where needed, and a reply that is checked and then sent.

- The preparation preamble now ends with an `ask_user` call: 2 to 4 short response options in the source language, e.g. Approve, Reject and Ask for details. This reuses the Question card (client/work-inbox), which already has choice buttons and a free-text answer, and works whether or not generated surfaces are on. With generated surfaces on, the model may send a form instead, as the runtime instructions already allow.
- The answer returns to the same Run. The preamble tells it to write the response, check it against the source, and for mail call `mail.send` with `reply_to` set to the source id. `mail.send` always needs approval (client/mail-calendar), and in an Unattended run anything above read also needs approval (policy.unattended.allowed_risk). So the Approval card, which shows the exact recipients, subject and body, is the "check before sending" step, and the safety boundary stays in policy, not in the preamble.
- Teams and calendar items: the client cannot send Teams messages, so the Run shows the final text to copy and sends no mail.
- Nothing new is written to Outlook before the user's approval, as D2 promised.

### D7. No action needed
The suggestion card in a triage Thread gets a No action needed button for open and started items. `TriageService::no_action(thread)` sets `suggestion='no_action'` (time in `updated_at`). Migration 2700 rebuilds triage_items so its CHECK allows that value, as 2400 did for the teams scope. The app then cancels the Thread's active Run, which drops the pending Question, and archives the Thread through the inbox store. A later revision keeps `no_action`, as for `started`. The digest only lists open and started action items, so the item leaves the Daily report. The card of an archived and restored Thread shows the status line with the decision.

This is client-side on purpose: it does not depend on the model offering the option, and it never goes back to the model.

### D8. One status badge
"Action required" and "Needs your action" meant the same thing. The triage badge is now only the category. The status is the existing `has_action` pill ("Needs your action"), which is also shown for a triage action Thread that has not had a Run yet (`last_run_state` empty), because it has no Action item and would otherwise have no status. A prepared Thread waits on its Question card, so it has an open Action item.

### D9. Collapsed source
The source is the first user message of the Run, stored with `provenance.trusted=false`, the preamble and the untrusted-data markers. The feed shows such a message as a collapsed Card, "Source: <kind>", whose body is the text between the markers, so the client's instruction text is not shown. This applies to every untrusted-start Run, Remote tasks included. Ownership: client/work-inbox, because it is feed rendering.

### D10. An invalid response no longer blocks the queue (acceptance feedback 2026-11-24)
On the user's laptop, triage stopped after 16:50 UTC with 620 events pending and six failures in a row, all "invalid response". Each step sends the oldest 20 pending events, and one malformed result fails the whole batch, so the same batch failed again after a doubling wait of up to an hour.

- After an invalid response, the next batch is half the size of the failed one (state `batch_size`), down to 1, and the next attempt waits only the usual one-minute spacing. The doubling wait stays for an unavailable gateway, where retrying sooner does not help.
- A single event that still gets an invalid response is set aside: it is stored as evaluated and uncertain with the reason "Triage could not classify this item.", through the same path as a model result, so an open Thread is withdrawn as for any uncertain result. A later revision of its source is evaluated again.
- A successful batch, or a set-aside, doubles the size again up to 20. Going straight back to 20 would put the failing event into a full batch again and repeat the cycle.
- The classifier's invalid-response message names the check that failed (for example "a relevant result has no valid category"), never a value from the response. It is logged and shown as the last error.
- A `null` action or category counts as left out, so a non-relevant result written with nulls is valid. D1 is otherwise unchanged.

With 20 events, an event that always fails is set aside within about ten attempts, a minute apart, while the events around it are evaluated in the smaller batches.

## Risks
- More model use: one preparation Run per action item, capped at 30 a day, and only for items already judged relevant.
- A small model may answer `info` for something that needs action. The reason is kept, and the item appears in the report, so it is not lost.
