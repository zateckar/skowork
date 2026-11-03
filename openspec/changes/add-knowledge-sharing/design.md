# Design

## Context

The knowledge graph (add-knowledge-graph) keeps everything in the encrypted kg.db on the laptop; the files Source indexes user-selected folders; settings offer Source toggles, folders typed as paths, exclusions and forgetting. The product owner decided on 2026-10-31: private by default, share chosen items with Groups through the control plane.

## Decisions

### D1. What can be shared
- **Choice:** documents, folders and notes only. These are things the user authored or deliberately imported. Mail, calendar, meetings and Threads contain other people's personal data and conversations; entities and facts are derived and can mix sources.
- **Alternatives considered:** sharing anything (personal data of third parties leaves the laptop by one click); sharing with individual users (more access-control surface; Groups already exist and are administered).

### D2. Server stores text and does its own indexing
- **Choice:** the client sends the extracted text of the item (not the file), title, kind, content hash and Groups. The control plane chunks it (same 1 KB chunks with overlap as the client), embeds chunks through the gateway's `embed` role on behalf of the owner, and stores chunks in SQLite with an FTS5 table and int8 vectors. Search embeds the query with the caller's `embed` role, takes the top 50 of each of FTS5 (bm25) and a vector scan limited to the caller's Groups, and fuses them with reciprocal rank fusion. No server-side reranking; the client reranks merged results as today.
- **Why:** the server cannot read files on laptops, and the client must not be trusted to supply vectors that match their text.
- **Alternatives considered:** clients upload their own embeddings (a client could plant vectors that surface unrelated text); a separate vector database (against "keep it simple").
- **Owner's Groups:** Group membership comes partly from IdP roles in the owner's token, which the server cannot read when the owner is offline. Each request of the owner (publish, list, search) stores their Groups and the Groups their policy permits; an item is returned for a Group only while that snapshot contains the Group in both. The client lists its shared items at start and at every policy refresh, so leaving a Group or a policy change takes effect at the owner's next policy refresh. A revoked owner (user deny-list entry) is hidden at once.
- **Sizing:** 2 MB text per item, default quota 200 MB of text per owner (server setting). A 1,000-user organisation sharing 50 documents each of 100 KB is about 5 GB of text plus about 1.3 GB of int8 vectors at 4096 dimensions; the vector scan of one Group is a small fraction of that.

### D3. Keeping shared items current
- **Choice:** the client re-publishes a shared item when its content hash changes after re-indexing, and unshares it when the file is deleted or excluded. A shared folder is stored on the client as a share rule; each supported file under it becomes its own shared item.

### D4. Knowledge view in the client
- **Choice:** a top-level Knowledge view in the navigation (beside the inbox views): a search box, Source tabs (Documents, Notes, Threads, Mail, Calendar, Meetings), an item list, and an item pane with text, entities and facts, sharing and actions. Import uses IFileOpenDialog (multi-select files; FOS_PICKFOLDERS for folders). Notes use the rich-edit control. The existing Settings > Knowledge section stays for Source toggles, budgets and exclusions.

### D5. Fact edits
- **Choice:** editing a fact deletes the extracted fact and stores a user-stated fact with the item as provenance; a tombstone (item id, item version, fact hash) keeps re-extraction of the same item version from restoring it.

### D6. Client details settled during implementation
- **Item ids and text:** a shared item's id in team knowledge is `k-` plus the content hash of its Source and local id, so re-publishing replaces it. A note is published as its title and text; a file as its extracted text (Office and pdf) or decoded text.
- **When it is published again:** after a shared item is re-indexed and at every sweep, when its item version changed and its text hash differs. Unchanged text sends nothing.
- **Unsharing without a connection:** the item is marked as being unshared and the DELETE is retried at each sweep.
- **Removed by an administrator:** the listing at start and at each policy refresh no longer contains the item. The client then shows it as no longer shared with the reason, and a share rule does not publish it again.
- **One file of a shared folder:** when the user unshares it, it stays out of the rule until they share it again.
- **Policy turns sharing off:** nothing more is published, and existing shares are left to the server, which stops returning them (D2).
- **No requests while nothing is shared:** with nothing shared and sharing off, the client sends no request to team knowledge.
- **Team passages:** a passage is cited as `workbench-team://<owner>/<item id>`. Its provenance is `{source_kind: team, owner, groups, trusted: false}`. Team passages take turns with local ones before reranking. knowledge.search only queries team knowledge when the `sources` filter is empty or contains `team`.
- **Notes:** notes are chunked in the host (they are typed by the user, not read from files). The notes Source lists every note, whether indexed or not.
- **Tombstones:** a tombstone is keyed by the item's Source, id and version (the file's size and modification time, or the note's content hash) and the normalised subject, predicate and object.

## Risks / Trade-offs

- [A user shares confidential content with a large Group] → Sharing is off until policy.knowledge.sharing.groups permits Groups; the confirmation shows title, size and Groups; admins can review and remove; every action is audited.
- [Shared text contains instructions aimed at colleagues' agents] → Team passages are untrusted data with owner provenance, exactly like local documents (client/policy-enforcement).
- [The owner leaves the organization] → Revocation of the user stops their items being returned; admins can remove them.

## Acceptance (tasks 2.3 and 3.1, 2026-11-01)

Local stack with the organization Keycloak user dev-user in a development client, and peer1 signed by a local test issuer, both members of the Group acceptance; policy.knowledge.sharing.groups permitted acceptance. A platform-admin test user (padmin, not in acceptance) ran the admin steps.
- Imported a Markdown document (after allowing its folder in policy.filesystem.read: the default policy refused it) and shared it with acceptance.
- peer1's team search returned the passage with owner dev-user and Group acceptance; padmin's search returned nothing.
- Unshare: peer1 found nothing and the client listed no shares. Shared again: found again.
- Changing the file: the re-indexed text was published again and peer1 found the new sentence within a minute.
- Sharing with everyone was refused with GROUP_NOT_PERMITTED.
- Admin removal with a reason: peer1 found nothing; the client showed the item as removed by an administrator at its next sweep, about a minute after start.
