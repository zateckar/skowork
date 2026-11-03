# Proposal

## Why

On 2026-11-01 the product owner called the Knowledge view clunky and noisy. Everything is stacked in one column:
- an unlabelled search box;
- three import buttons;
- the source tabs;
- imported folders and documents shown as raw paths with loose Remove buttons;
- item rows that are a line of text with a separate Open button;
- the opened item, at the very bottom below the whole list.

Finding and reading an item takes scrolling, and the screen shows management controls most users rarely need.

## What Changes

- The view gets two panes like the Inbox: the list on the left and a reader on the right. On a narrow window the reader replaces the list, with Back.
- One search field with a placeholder at the top. Results replace the list and keep their snippets.
- One Add menu replaces the three buttons: Import documents, Import folder, New note.
- The Source tabs become a compact row with item counts.
- Each row is the item itself: its title, then one muted line with the date, the size and badges (Shared, Indexing). A click opens it in the reader.
- The reader puts the title and the actions first: Share or Unshare, Edit for notes, and a More menu with Exclude and Forget. Then come the indexed text and a collapsed Details section with entities and facts.
- Imported folders and documents move to a Manage imports panel opened from the Files tab, where they can be shared and removed.

Out of scope:
- What is indexed, retrieval, sharing rules and curation behaviour (unchanged).
- Saving mail and Chats as notes (add-save-as-note).

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/knowledge-graph`: Knowledge View (modified).

## Impact

- **Roadmap:** 21, after add-offline-operation. Depends on add-knowledge-graph and add-knowledge-sharing.
- **Code:**
  - client/src/shell: the Knowledge view's layout, list rows, reader and the Manage imports panel;
  - strings in en, de, cs;
  - the Knowledge view reference image.
- **Data:** none. The knowledge.* RPCs are unchanged, except item counts per Source, which knowledge.browse returns.
