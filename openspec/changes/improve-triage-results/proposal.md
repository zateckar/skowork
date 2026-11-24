# Triage results instead of suggestions

## Why
Triage turns relevant mail, calendar events and Teams messages into Threads titled "Suggestion: …" with a "Skowork" origin badge and a Start work button. On a real mailbox this gives a list of identical-looking Threads. The badge says nothing, the prefix repeats on every title, and the user still has to open each one and start the work by hand. The user wants each relevant item processed automatically. The result should say either "for your information" or "action required", and for an action, the draft of that action should be ready to review.

## What Changes
- Classification also returns, for a relevant item, whether it needs action from the user or is for information, and one topic category: request, approval, question, meeting, deadline, decision or update.
- An item that needs action gets a Thread titled with the source subject, with no prefix. A read-only Unattended preparation Run starts automatically in it and writes a summary and, where the item calls for a response, a draft. The draft stays in the Thread: nothing is sent, and no mail draft is created in the mailbox. The user can still dismiss it. Automatic preparation can be turned off (default on) and is capped per day; above the cap, or when policy does not let Unattended runs read, the Thread waits with Start work as before.
- An item for information gets no Thread. It is listed with a one-line summary in the next Daily report.
- Triage Threads show the category badge instead of the system origin badge, and one status badge, "Needs your action".
- After the summary, the preparation asks for the decision on a Question card with clickable options and a free-text answer. The user's choice comes back to the Run, which writes and checks the reply and requests mail.send. Nothing is sent until the user approves the exact mail. Teams items get the text to send by hand.
- No action needed records the decision, cancels the preparation and archives the Thread.
- The source message that starts a Run from untrusted content is collapsed in the feed, so the summary is visible at once.
- The Daily report receives the open triage action items, which it was meant to list but never did.

## Impact
- Specs: client/event-triage (changed by improve-proactive-durable-work and add-teams-triage, both in flight), client/work-inbox, client/daily-report (added by add-local-outlook-and-teams, in flight).
- Code: `client/src/triage` (classifier, service), `client/src/app/app.cpp` (preparation runs, report digest), `client/src/shell` (badges, suggestion view), `client/src/report` (digest), strings.
- Data: triage_items gains action and category columns (migration).
