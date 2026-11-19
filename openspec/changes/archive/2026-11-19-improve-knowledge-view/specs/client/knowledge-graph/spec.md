# Spec Delta

## MODIFIED Requirements

### Requirement: Knowledge View
The client SHALL offer a Knowledge view, reachable from the navigation, made of a list pane and a reader. In the view the user can:
- search the knowledge base with the same retrieval the agent uses, from one search field at the top; the results replace the list until cleared;
- add knowledge from one Add menu: import documents through the system file picker, import a folder through the system folder picker, or write a new note;
- browse items by Source, where each Source tab shows its item count and each row shows the item's title with its date, size and state (shared, indexing) on a second line;
- open an item in the reader with one click and read its title, Source, date, size, sharing state and indexed text, with its entities and facts in a Details section that starts collapsed;
- manage imported folders and documents in a separate panel of the Files Source.
On a narrow window only one pane SHALL show at a time, and the reader offers Back to the list.

#### Scenario: Search from the view
- **WHEN** the user searches for "Falcon" in the Knowledge view
- **THEN** the list pane shows the matching passages with their Source, item title, date and snippet, and each opens its item in the reader

#### Scenario: Open an item
- **WHEN** the user clicks a document row in the Files tab
- **THEN** the reader shows its title, meta line, sharing state and text next to the list, and the row is marked as selected

#### Scenario: Narrow window
- **WHEN** the window is narrow and the user opens an item
- **THEN** the reader replaces the list, and Back returns to the list at the same position
