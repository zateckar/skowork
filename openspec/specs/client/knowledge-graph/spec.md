# client/knowledge-graph Specification

## Purpose
The user's local, private knowledge store on the laptop: what it ingests (Threads, files, mail, calendar, meetings and the user's notes), how it is indexed and searched with provenance, and how the user keeps notes in it, including notes saved from a mail item or a Chat. Knowledge leaves the laptop only as LLM context through the gateway, or when the user shares it with a Group.

## Requirements

### Requirement: Save As Note
The client SHALL let the user save an indexed mail item, or the messages of a Chat over a chosen period (the last 24 hours, the last 7 days, or everything kept on the computer), as a note under Knowledge Notes. It opens the note editor filled with the title, a header (sender, recipients and date, or participants and period) and the text, and stores nothing until the user saves. With policy.knowledge.save_as_note off (on or off, default on), no Save as note action is offered.

#### Scenario: Save a Chat
- **WHEN** the user chooses Save as note for a team Chat with the period Last 7 days
- **THEN** the editor holds the messages of the last 7 days with their senders and times, and nothing is stored until the user saves

#### Scenario: Switched off by policy
- **WHEN** policy.knowledge.save_as_note is off
- **THEN** neither mail items nor Chats offer Save as note

### Requirement: Saved Note Origin
A note saved from mail or a Chat SHALL be private by default, editable and shareable under Sharing Knowledge Items like any note. It records where it came from, and the Knowledge view shows that origin with a link to the mail item or Chat. Sharing such a note SHALL say in the confirmation that it contains copied mail or Chat content.

#### Scenario: Share a note saved from mail
- **WHEN** the user saves a mail item as a note and shares the note with a permitted Group
- **THEN** the note shows that it was saved from mail, and the share confirmation says it contains copied mail content

### Requirement: Local-Only Knowledge Store
The client SHALL keep chunks, indexes, embeddings, entities, relations and facts in the user's data root. Uploads are limited to model requests through client/model-access, which return results but store nothing on the server, and explicitly shared item text sent to control-plane/team-knowledge under Sharing Knowledge Items.

#### Scenario: No knowledge sync
- **WHEN** the knowledge store contains indexed items and the user has shared none of them
- **THEN** no request from the client to any control-plane service other than the LLM gateway contains chunk text, embeddings or extracted facts

#### Scenario: Only shared items leave the laptop
- **WHEN** the user shares one document of an indexed folder
- **THEN** the text of that document is sent to control-plane/team-knowledge, and nothing of the other documents, entities or facts is sent

### Requirement: Knowledge View
The client SHALL offer a Knowledge view from navigation, with a list pane and reader. Users can search, add and browse knowledge by Source, inspect items and manage imported files. Narrow windows show one pane at a time and the reader offers Back.

#### Scenario: Search from the view
- **WHEN** the user searches for "Falcon" in the Knowledge view
- **THEN** the top search field uses the agent's retrieval and replaces the list until cleared; matching passages show Source, item title, date and snippet, and each opens its item in the reader

#### Scenario: Open an item
- **WHEN** the user clicks a document row in the Files tab
- **THEN** the reader shows its title, Source, date, size, sharing state and indexed text, with entities and facts in initially collapsed Details, and the row is selected; Source tabs show counts and rows show title, date, size and shared or indexing state
- **AND** the Add menu offers document and folder system pickers and a new note, and Files has a separate imported folder and document management panel

#### Scenario: Narrow window
- **WHEN** the window is narrow and the user opens an item
- **THEN** the reader replaces the list, and Back returns to the list at the same position

### Requirement: Knowledge Curation
In the Knowledge view the user SHALL be able to edit and delete a fact, rename an entity, and forget or exclude an item. Edited facts are stored as user-stated. A deleted fact MUST NOT be extracted again from the same item version.

#### Scenario: Correct a fact
- **WHEN** the user edits the fact "Alice works in Sales" to "Alice works in Engineering"
- **THEN** knowledge.entity for Alice returns the edited fact as user-stated, and not the old one

### Requirement: Sharing Knowledge Items
Every knowledge item SHALL be private by default. The user MAY share an imported document, a selected folder or a note with their Groups that policy.knowledge.sharing.groups permits (Group names or `*`; default empty), after confirming title, size and Groups. The shared text goes to control-plane/team-knowledge and is kept current. The user can see what is shared and unshare at any time. Mail, calendar, meeting and Thread items, entities and facts MUST NOT be shareable.

#### Scenario: Share a document
- **WHEN** policy.knowledge.sharing.groups includes the Group engineering, and the user shares an imported document with engineering and confirms
- **THEN** the document is listed as shared with engineering, and members of engineering can find it with knowledge.search

#### Scenario: Unshare
- **WHEN** the owner unshares the document
- **THEN** it is removed from the team knowledge space, and searches by other users no longer return it

#### Scenario: Sharing not permitted
- **WHEN** policy.knowledge.sharing.groups is empty
- **THEN** the Knowledge view offers no share action and explains that sharing is not enabled by policy

#### Scenario: Mail cannot be shared
- **WHEN** the user opens an indexed mail item in the Knowledge view
- **THEN** no share action is offered for it
