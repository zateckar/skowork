# Design

## Context

The Knowledge view (client/src/shell/views_knowledge_view.*) is one vertical stack, rebuilt on every change. It calls these RPCs:
- knowledge.browse, to list the items of a Source;
- knowledge.item and knowledge.note.get, to open an item or a note, with knowledge.note.save and knowledge.note.delete;
- knowledge.search;
- knowledge.document.add and knowledge.folder.add, with knowledge.status for the imported folders and documents;
- knowledge.share and knowledge.unshare, with knowledge.share_folder and knowledge.unshare_folder for folders;
- knowledge.document.remove;
- knowledge.exclusion.add;
- knowledge.item.forget.

The Inbox uses a list pane and a reading pane with a splitter (client/work-inbox Three-Pane Layout).

## Decisions

### D1. Two panes inside the page
- **Choice:**
  - The page content is a horizontal split: a list pane (360 DIPs by default, resizable from 280 to 560, kept while the window is open, as the Inbox list width is) and the reader.
  - Below 900 DIPs of page width only one pane shows. Opening an item shows the reader with a Back button, which returns to the list with the same scroll position.
  - The reader shows a quiet empty state ("Select an item to read it") until an item is opened.
- **Alternatives considered:**
  - A dialog for the item, which covers the list and cannot be kept open while browsing.
  - Expanding rows in place, which makes long lists jump.

### D2. Header: search and Add
- **Choice:**
  - One row holds the search field, with the placeholder "Search your knowledge" and Enter to search, and an Add button with a menu: Import documents…, Import folder…, New note.
  - While results are shown, the list pane lists them under "Results for "…"" with a Clear button. Each result shows title, Source, date and a two-line snippet.
  - The intro sentence becomes a one-line caption under the page title.
- **Alternatives considered:** keeping three buttons, which repeats today's noise.

### D3. Source tabs with counts
- **Choice:** the tabs are subtle buttons in one row above both panes, with the selected one marked and the count after the label ("Notes 3"). knowledge.browse returns `counts` per Source next to the items.
- **Alternatives considered:** a drop-down, which hides the Sources.

### D4. Rows
- **Choice:**
  - A row is one list item, as tall as two lines. The first line is the title. The second, muted line holds the date, the size and badges: Shared (with the Groups in the tooltip), Indexing, and Excluded, which is not listed at all.
  - A click or Enter opens the item. The open item's row is marked as selected.
  - The accessible name joins title, date, size and badges.
- **Alternatives considered:** a table with columns, which is too wide for the list pane.

### D5. Reader
- **Choice:**
  - Title, then the meta line (Source · date · size), then an action row:
    - Share or Unshare when sharing applies;
    - Edit for notes;
    - More (…) with Exclude… and Forget…, each confirmed as today.
  - Then the sharing state in one line, the indexed text (read-only), and a Details expander, collapsed by default, with entities (rename) and facts (edit, delete).
  - The note editor opens in the reader instead of above the list.
- **Alternatives considered:** keeping entities always open, which is the noisiest part for most items.

### D6. Manage imports
- **Choice:**
  - The Files tab shows a "Manage imports" link above its rows. It opens a panel in the reader that lists the imported folders and the individually imported documents.
  - Each folder has Share folder or Unshare folder and Remove; each document has Remove.
  - Paths are shown as the folder or file name with the full path as a muted second line.
- **Alternatives considered:** moving imports to Settings, which separates them from the items they produce.

## Risks / Trade-offs

- [Tests and automation ids of the old layout break] → The ids of the actions are kept (knowledge-share, knowledge-item-forget, knowledge-note-save and so on); the shell tests are updated in the same task.
- [A narrow window hides the reader] → The single-pane mode with Back keeps both reachable.

## Acceptance (2.1)

- **Reference image:** shell_knowledge.png shows the Documents tab with three rows (Shared and Indexing badges) and an open document in the reader, with the Details expander collapsed. Review found "1 facts" in the Details header; the entity and fact counts are now plural forms in en, de and cs.
- **Development client** (dark theme, demo data):
  - Documents 1, Notes 1, Threads 10, Mail 0, Calendar 0, Meetings 0. The reader shows "Select an item to read it." until a row is opened.
  - Opening brake-calibration-notes.md shows the meta line, Share… and More, "Private: only you can find this item.", the text, and "Details: 4 entities, 1 fact".
  - The Threads tab lists ten Threads in a scrolling list; an open Thread shows only More, with "Only documents, folders and notes can be shared."
