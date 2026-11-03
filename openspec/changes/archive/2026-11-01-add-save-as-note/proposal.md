# Proposal

## Why

On 2026-11-01 the product owner asked whether emails and Chats can become knowledge documents that are shared with colleagues. Today mail items are indexed for the user alone, and mail, calendar, meeting and Thread items are deliberately not shareable (client/knowledge-graph Sharing Knowledge Items). Chats are not a knowledge Source at all. Sharing the content of a decision made in a mail or a Chat means copying it by hand into a note.

The product owner chose a snapshot note: a copy the user reviews and owns, which never changes behind their back.

## What Changes

- A mail item opened in the Knowledge view offers Save as note. An open Chat offers Save as note in its action row, with a period: the last 24 hours, the last 7 days, or everything kept locally.
- Save as note opens the note editor filled in, and nothing is stored until the user saves:
  - the title: the mail subject, or the Chat name with the date;
  - a header: From, To, Date for mail; the participants and the period for a Chat;
  - the text as Markdown.
- The saved note is an ordinary note: private, editable, indexed, and shareable like any note. The reader shows where it came from, with a link that opens the mail item or the Chat.
- Sharing a note saved from mail or a Chat shows one extra line in the confirmation: that it contains copied mail or Chat content.
- policy.knowledge.save_as_note (on or off, default on) lets administrators switch the feature off.

Out of scope:
- Keeping the note in sync with the mail or Chat.
- Agent tools for saving notes.
- Saving Threads, calendar events or meetings as notes.
- Mail withheld because of its sensitivity label, which is never indexed and cannot be opened (client/mail-calendar Sensitivity Label Exclusion).

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/knowledge-graph`: Save As Note (added).

## Impact

- **Roadmap:** 22, after improve-knowledge-view. Depends on:
  - add-knowledge-graph, for notes;
  - add-knowledge-sharing;
  - add-mail-calendar, for indexed mail items;
  - add-user-messaging, for Chats;
  - improve-knowledge-view, for the reader that holds the action.
- **Code:**
  - client/src/knowledge (a note's origin, a draft from a mail item or a Chat);
  - client/src/shell (actions in the reader and the open Chat, the period choice, the extra confirmation line);
  - control-plane/src/policy/keys.ts (the new key) and the policy schema fixture;
  - strings in en, de, cs.
- **Data:** notes gain an origin (kind and reference), added by a kg.db migration.
