# Design

## Context

- Notes are stored in the knowledge store and indexed as the `notes` Source.
- Indexed mail items keep their text, subject, sender, recipients and date in the item's metadata.
- Chats and their recent messages are cached in app.db by the messaging service (client/user-messaging Offline Sending). Their age is limited by that cache: it keeps the recent messages, not the whole history.

## Decisions

### D1. A draft, then an ordinary note
- **Choice:**
  - New RPCs `knowledge.note.draft_from_mail {item_id}` and `knowledge.note.draft_from_chat {chat, period: day|week|all}` return `{title, text, origin}` without storing anything.
  - The reader opens the note editor with that draft. Saving calls `knowledge.note.save` with `origin`; the note stores `origin_kind` (mail | chat) and `origin_ref` (the item's external id, or the Chat id).
  - The note also stores `origin_title` (the subject or the Chat name at the time), so the origin line can be shown after the source is gone.
  - `knowledge.note.get` and the reader's item return the origin with `item_id` (the mail item, 0 when it is no longer indexed) or `available` (whether the Chat is still cached), which decide whether the origin line is a link.
  - Editing keeps the origin. Deleting the note deletes nothing else.
- **Alternatives considered:**
  - Saving at once, which stores content the user did not look at.
  - A live item that follows the source, which the product owner declined.

### D2. Note text
- **Choice:**
  - Mail: a heading line with the subject, then `From:`, `To:`, `Cc:` (if any) and `Date:` lines, a blank line, and the body as plain text.
  - Chat: the Chat name and the dates it covers as the title, a `Participants:` line and a `Period:` line, then one paragraph per message: `**Name** (yyyy-mm-dd hh:mm UTC): text`. Messages keep their Markdown. Pending messages are left out. A period without messages gives a note that says so.
  - Times are written in UTC, because the note is shared with colleagues who may be in other time zones.
  - The text is cut at the note size limit with a closing line saying it was shortened.
- **Alternatives considered:** HTML of the mail, which the knowledge store does not keep.

### D3. Where the actions are
- **Choice:**
  - Mail items: Save as note in the reader's action row, shown when policy.knowledge.save_as_note is on.
  - Chats: a Save as note… button in the open Chat's action row (team and direct Chats), with a small dialog to choose the period (Last 24 hours, Last 7 days as the default, Everything on this computer). The Chat view has no header menu, so the action sits with Rename and Add members.
  - The new note opens in the Knowledge view's reader with the editor. The editor shows the origin line and, until the draft is saved, a line saying nothing is stored yet.
- **Alternatives considered:** a context menu per message, a finer choice that adds a selection model to the Chat view for little gain.

### D4. Origin in the reader and when sharing
- **Choice:**
  - The reader of a note with an origin shows "Saved from mail: <subject>" or "Saved from Chat: <name>". The text is a link that opens the mail item or the Chat; if the source is gone, the line is plain text.
  - The share confirmation of such a note adds "This note contains copied mail content." or "…copied Chat messages."
- **Alternatives considered:** blocking the sharing of copied content, which the request asks for.

### D5. Policy key
- **Choice:** `policy.knowledge.save_as_note`, an enum of on and off with default on, merged as a scalar, owned by client/knowledge-graph. When it is off, the actions are not shown and the draft RPCs refuse with `not_permitted`. Notes saved earlier stay.
- **Alternatives considered:** reusing policy.knowledge.sharing.groups, which governs sharing, not copying into private notes.

## Risks / Trade-offs

- [Confidential mail content is copied into a shareable note] → The user reviews the draft before saving. The share confirmation names copied content. Mail withheld by sensitivity label is never available. Administrators can switch the feature off.
- [A long Chat produces a huge note] → Notes are capped at the note size limit, with a visible "shortened" line.

## Implementation Notes

- The Knowledge and Chats views close or rebuild themselves from an action's completion callback (Save as note navigates to the Knowledge view). Both views now check that they still exist after that callback; before, the Chats view rebuilt itself after it had been destroyed (found by the hidden-window UI test).
- The acceptance run drove the app through UI Automation and crashed it twice, in a rebuild and in painting. UI Automation calls the providers on its own threads, so the element tree was changed from two threads at once. The providers now send each call to the window thread (add-native-ui design, Toolkit findings). This also affected screen readers.

## Acceptance (2026-11-01)

On the local stack (control plane, dev proxy, Microsoft Graph mock as the mailbox, the debug client and the peer user peer1 in the acceptance Group), driven through UI Automation:
- Mail tab → "Board pack – strictly confidential" → Save as note: the editor opened with the subject as the title, From, To and Date (UTC) lines and the body, the origin line and the draft hint. Nothing was stored until Save; Save stored note 3 with its origin.
- Chat "Acceptance team (renamed)" → Save as note… → Everything on this computer: a note with the participants, the period and the four messages, then saved as note 4.
- Share on note 4: the confirmation named the copied Chat messages, with acceptance preselected. After sharing, peer1's `POST /api/v1/knowledge/search` for "Missed message two team reply" returned the note (kind notes, owner dev-user, groups acceptance).
- The reader's "Saved from Chat: Acceptance team (renamed)" link opened that Chat.
- The run showed two blank lines before the first message of a Chat note; the text now has one, and the knowledge test checks it.
