# Spec Delta

## MODIFIED Requirements

### Requirement: Local-Only Knowledge Store
The client SHALL keep all knowledge data (chunks, indexes, embeddings, entities, relations and facts) only in the user's data root. It MUST NOT upload knowledge data except in model requests through client/model-access, which return results but store nothing on the server, or as the explicitly approved content of Sharing Knowledge Items and Category Publication to control-plane/team-knowledge. Category membership and private dependencies are not permission to upload them.

#### Scenario: No knowledge sync
- **WHEN** the knowledge store contains indexed items and the user has shared none of them
- **THEN** no request to a control-plane service other than the LLM gateway contains their text, embeddings, facts or categories

#### Scenario: Only shared items leave the laptop
- **WHEN** the user shares one document of an indexed folder
- **THEN** that document's text is sent to control-plane/team-knowledge and nothing from the other documents or their derived knowledge is sent

#### Scenario: Publish one category selection
- **WHEN** the user approves selected facts and excerpts from a category
- **THEN** only the previewed publication content is uploaded and no private graph, complete source document or unselected evidence accompanies it

### Requirement: Knowledge View
The client SHALL offer a Knowledge view with Sources, Knowledge, Review and Sharing sections, a searchable list and a reader. Users can import documents or folders through system pickers, write notes, search with the agent's retrieval, browse by Source and category, and inspect text, extracted facts, evidence, status and sharing. Source counts, item date and size, and import management remain available. On narrow windows, the reader replaces the list and Back preserves its position.

#### Scenario: Search from the view
- **WHEN** the user searches for Falcon
- **THEN** matching passages and facts show their Source, item title, date, category and knowledge state, and each opens the corresponding reader

#### Scenario: Open an item
- **WHEN** the user selects a document in Sources
- **THEN** the reader offers Content, Extracted knowledge, Changes and Sharing, with the selected item visibly marked

#### Scenario: Item responses arrive out of order
- **WHEN** the user opens another item or refreshes the current item while an earlier item request is still in flight
- **THEN** an older response cannot replace the most recently requested reader content or display its error against the newer selection

#### Scenario: Source list refreshes overlap
- **WHEN** a Source list is refreshed while an earlier request for the same Source is in flight
- **THEN** the list and its counts show the latest request's result even if the older response arrives last

#### Scenario: Leave a pending search
- **WHEN** the user changes Source or clears a search while its request is in flight
- **THEN** that request cannot replace the selected view with results or an error after it returns

#### Scenario: Narrow window
- **WHEN** the window is narrow and the user opens an item
- **THEN** the reader replaces the list and Back returns to the previous selection, filters and scroll position

### Requirement: Knowledge Curation
The Knowledge view SHALL let the user edit or reject a fact, rename, merge or split an entity, and forget or exclude an item. Corrections are user-stated annotations with the original evidence retained as provenance; re-extraction MUST NOT overwrite them. A rejected fact MUST NOT reappear from the same item version. A changed version that challenges a correction SHALL create a review entry.

#### Scenario: Correct a fact
- **WHEN** the user corrects Alice works in Sales to Alice works in Engineering
- **THEN** knowledge.entity shows the correction as user-stated with its evidence, and the old fact is not presented as current

#### Scenario: New source contradicts a correction
- **WHEN** a new document version supports Sales after the user's correction to Engineering
- **THEN** the correction is preserved and the disagreement enters Review with both versions and their evidence

### Requirement: Sharing Knowledge Items
Every knowledge item SHALL be private by default. The user can share an imported document, selected folder or note with their Groups permitted by policy.knowledge.sharing.groups (Group names or `*`; default empty), after confirming title, size, Groups and update behavior. These explicit item shares remain current and can be unshared. Raw mail, calendar, meeting and Thread items and whole entities MUST NOT be shared. Facts can leave the device only under Category Publication.

#### Scenario: Share a document
- **WHEN** policy.knowledge.sharing.groups permits engineering and its member confirms sharing an imported document there
- **THEN** engineering members can find it with knowledge.search and the owner sees that changes to the document will also be shared

#### Scenario: Unshare
- **WHEN** the owner unshares a document and the server acknowledges removal
- **THEN** it is removed from team knowledge and other users' new searches no longer return it

#### Scenario: Sharing not permitted
- **WHEN** policy.knowledge.sharing.groups is empty
- **THEN** sharing controls explain that policy has not enabled sharing and do not publish anything

#### Scenario: Source access is revoked before an item upload
- **WHEN** an indexed document or note is excluded or its Source is disabled by policy before an item-sharing upload starts
- **THEN** the client does not upload its content even if background cleanup has not yet removed the indexed item
- **AND** existing shares of the inaccessible item are queued for withdrawal

#### Scenario: Mail cannot be shared
- **WHEN** the user opens an indexed mail item
- **THEN** no raw-mail share action is available; any derived fact must separately meet Category Publication eligibility

## ADDED Requirements

### Requirement: Source Inventory
Sources SHALL show enabled kinds and imported folders, documents or connected items, with indexing, extraction and consistency status, last successful revision, pending work, errors and sharing state. Pausing, removing an import and forgetting knowledge SHALL be distinct and explained. Temporary access or indexing failure MUST NOT be treated as deletion; retained knowledge remains subject to policy, and the view and agent search SHALL indicate uncertain freshness.

#### Scenario: Previously indexed document cannot be extracted
- **WHEN** an imported document changes but text extraction reports that the new content is corrupt or unavailable
- **THEN** the last successfully indexed passage remains available as historical text with uncertain freshness and the failure is reported
- **AND** facts whose only support changed are not presented as current until rechecked
- **AND** a later successful extraction replaces that knowledge normally, while policy revocation and explicit forgetting still remove it

#### Scenario: Paused folder
- **WHEN** the user pauses indexing of a selected folder
- **THEN** its current knowledge remains readable, its freshness is shown as paused, and the UI does not claim that its current files have been rechecked

#### Scenario: User switches off file indexing
- **WHEN** the user switches off the files Source without forgetting its knowledge
- **THEN** previously indexed permitted files remain readable in the Knowledge view and agent search
- **AND** the user and agent can tell that file knowledge may be out of date until a complete recheck succeeds after indexing resumes

#### Scenario: File source temporarily unavailable
- **WHEN** an indexed folder cannot be read or a document indexing worker is temporarily unavailable
- **THEN** the last indexed knowledge remains subject to current policy, the user and agent are told it may be out of date, and confirmed deletions in accessible folders still reconcile

#### Scenario: File freshness changes during search
- **WHEN** an indexed file source becomes unavailable while a search is waiting for a model response
- **THEN** the returned search reports uncertain file freshness even if the source was healthy when the search started

#### Scenario: File changes during indexing
- **WHEN** an imported file changes while its content is being read or extracted
- **THEN** the in-flight result is discarded, the last indexed knowledge remains subject to current policy, and the Source indicates that another indexing attempt is needed

#### Scenario: Read permission changes during indexing
- **WHEN** policy revokes read permission for an imported file while its content is being indexed
- **THEN** the in-flight result is discarded and previously indexed knowledge for that file is removed from retrieval

### Requirement: Evidence For Every Fact
Every extracted fact SHALL identify its supporting item revisions, exact excerpts and locators, extraction time and review status. A combined fact SHALL retain every independent support and every required dependency. The user can move from a fact to its evidence and from a Source item to everything derived from it.

#### Scenario: Two documents support one fact
- **WHEN** two documents independently state the same project start date
- **THEN** Knowledge shows one fact with two supporting Sources, and each evidence link opens the matching excerpt of its recorded revision

#### Scenario: Analysis cites an exact excerpt
- **WHEN** category or inconsistency analysis cites a passage span for a fact
- **THEN** the client binds the span only if the analyzed graph generation is still current, the passage belongs to a current support of that fact, and the complete excerpt matches the current revision
- **AND** the resulting excerpt remains tied to that support and is treated as evidence for a suggestion rather than proof of correctness

### Requirement: Honest Knowledge States
The client SHALL distinguish current extracted facts, user-confirmed or corrected facts, disputed facts, superseded facts and facts with no remaining support. Model confidence MUST NOT be displayed as a probability that a fact is true, and incomplete extraction or checking MUST NOT be presented as a complete assessment.

#### Scenario: Extraction pending
- **WHEN** a new Source has been indexed but extraction or consistency checking is queued
- **THEN** its reader shows the pending stages and does not report zero extracted facts or no inconsistencies as a final result

#### Scenario: Extraction request or response fails
- **WHEN** a model request is rejected or its response is malformed
- **THEN** affected passages remain pending for retry and the Source reader does not claim extraction finished
- **AND** the reader explains that analysis is delayed and that the Source remains searchable

#### Scenario: One passage repeatedly prevents extraction
- **WHEN** a batch has a content-specific failure and an individual passage continues to fail
- **THEN** the client temporarily defers that passage with bounded retry and continues eligible unrelated passages within the analysis budget
- **AND** it retains the failed passage in the queue and identifies its Source as delayed until that passage succeeds or is removed
- **AND** service-wide failures and quota limits retain a shared backoff rather than sending repeated requests for each passage

### Requirement: Inspectable Category Assignment
The client SHALL automatically assign existing categories when policy.knowledge.categorization is on (on|off; default on) and extraction is enabled. Assignments show that they are automatic and why; new category names are suggestions. Users can create, rename, merge and remove categories, assign or reject labels and lock manual choices. Uncategorized remains available, and removing a category never deletes its knowledge.

#### Scenario: Correct an automatic category
- **WHEN** the user moves a fact from Sales to Engineering and rejects Sales
- **THEN** later extraction preserves that manual choice unless the user resets it

#### Scenario: Categorization unavailable
- **WHEN** categorization is off, extraction is off or the model is unavailable
- **THEN** manual categorization and search still work and new unclassified knowledge appears in Uncategorized

#### Scenario: Merge categories with manual decisions
- **WHEN** categories are merged and a fact has decisions in both categories
- **THEN** a manual decision takes precedence over an automatic assignment, and the destination's manual decision wins if both are manual
- **AND** no facts or evidence are deleted, and published selections do not change

#### Scenario: Reset a manual category decision
- **WHEN** the user resets a manual assignment or rejection
- **THEN** later automatic suggestions can propose the category again, and facts without accepted categories appear in Uncategorized

### Requirement: Category Membership Is Not Access
Category membership SHALL organize local knowledge without granting access to it. A fact can belong to multiple categories, and automatic or manual changes to category membership MUST NOT add content or recipients to an existing publication without its owner's confirmation.

#### Scenario: New item in a shared category
- **WHEN** automatic categorization adds a private fact to a category with a published selection
- **THEN** the fact stays private and Sharing lists it as an unpublished addition

### Requirement: Possible Inconsistency Review
With policy.knowledge.consistency on (on|off; default on) and extraction enabled, the client SHALL check new or changed facts against relevant existing facts and list possible inconsistencies with both excerpts, dates and scope. It SHALL distinguish incompatible claims from different time periods and uncertain entity matches, and never select a winner solely because one Source is newer.

#### Scenario: Different project deadlines
- **WHEN** two current Sources state incompatible deadlines for the same project and milestone
- **THEN** Review presents both values and evidence as a possible inconsistency, with the checked scope and remaining backlog visible

#### Scenario: Historical role change
- **WHEN** one Source says Alice led a project in 2024 and another says Bob leads it from 2025
- **THEN** the dates are preserved and the facts are not treated as contradictory merely because their names differ

### Requirement: User Resolution Of Inconsistencies
Review SHALL let the user keep both facts with distinct scope, prefer one with a reason, correct a fact or defer the decision. A resolution is tied to the evidence revisions reviewed, is reversible while its evidence remains, and is reopened when relevant new evidence invalidates its basis.

#### Scenario: Resolve and reopen
- **WHEN** a user prefers an approved project plan over a draft and the approved plan later changes the relevant passage
- **THEN** the old resolution remains explainable in history and the changed evidence is presented for review instead of inheriting the decision silently

#### Scenario: Reviewed evidence becomes unavailable
- **WHEN** evidence underlying a resolution is invalidated before consistency analysis can check its replacement
- **THEN** Review marks the prior decision as needing recheck rather than treating the old resolution as current or claiming a new disagreement was verified

### Requirement: Knowledge Impact Preview
Before an explicit import, replacement, exclusion or forget operation is committed, the client SHALL show its estimated effects: added, supported, changed, disputed, unsupported or removed facts, category changes and affected shares. Incomplete analysis is marked pending with its limitations; confirming incomplete analysis cannot silently approve later conflict resolutions or publication additions.

#### Scenario: Import challenges existing knowledge
- **WHEN** an analyzed import would add 12 facts, corroborate 4 and dispute 2
- **THEN** its preview shows those counts and opens the affected facts and evidence before the user accepts or cancels the import

#### Scenario: Model unavailable during preview
- **WHEN** extraction cannot run for an import
- **THEN** the user can cancel or explicitly import for search with analysis pending, and the preview does not claim that existing knowledge is unaffected

#### Scenario: Partial item removal information
- **WHEN** an item removal confirmation can show only its currently listed facts and corrections
- **THEN** it labels the broader effects on other knowledge and shares as not fully analyzed and does not claim every derived fact will be removed

### Requirement: Revision-Safe Impact Confirmation
An impact confirmation SHALL apply only to the Source and knowledge revisions analyzed. If those revisions change before the operation commits, the client SHALL refresh the preview and require confirmation of materially changed effects.

#### Scenario: File changes while preview is open
- **WHEN** the file changes after the user reviewed its import preview
- **THEN** the client does not ingest the new bytes under the old confirmation

### Requirement: Incremental Source Reconciliation
A Source update SHALL invalidate evidence from changed or removed passages and recompute only affected derivations. Unchanged evidence and independent support remain valid. Automatic watched updates produce an inspectable change report; disputed or unsupported results are not silently treated as current facts.

#### Scenario: Content changes without metadata changing
- **WHEN** an imported file's bytes change while its size and modification time remain the same
- **THEN** the next successful check detects the new content and does not retain the old extraction as current knowledge

#### Scenario: Existing deleted-fact protections survive a fingerprint upgrade
- **WHEN** an existing file is first checked after the client upgrades from a size-and-time fingerprint to a content fingerprint
- **THEN** deleted-fact protections for the old fingerprint are carried forward conservatively when its recorded size and time match the file

#### Scenario: One source withdraws support
- **WHEN** a revised document removes a date supported independently by another current document
- **THEN** the fact remains with the surviving evidence and the removed support appears in the change report

#### Scenario: Only support changed
- **WHEN** a fact's sole supporting passage is replaced and extraction is temporarily unavailable
- **THEN** the old fact is marked unsupported or awaiting recheck and is excluded from current-fact answers until valid support is established

### Requirement: Forget Dependency Cascade
Forgetting a Source or item SHALL remove its text, excerpts, chunks, embeddings, extraction results and derived dependencies under client/local-storage. Facts with independent surviving evidence remain; unsupported facts and dependent summaries stop being used. Related publication withdrawals are durably queued. Forgetting an item MUST NOT delete the original file, mail or calendar entry.

#### Scenario: Forget one of two supporting items
- **WHEN** the user forgets one document that supports a fact
- **THEN** that document's stored content and evidence disappear, the fact survives only if its other support suffices, and no history view restores the forgotten content

#### Scenario: Forget while indexing
- **WHEN** a user forgets an item or removes its last import while an indexing operation for it is in flight
- **THEN** that operation cannot restore its indexed content after the change is acknowledged

#### Scenario: Forget while extraction is in flight
- **WHEN** an extraction result arrives after one of its input passages was forgotten or replaced
- **THEN** that result does not recreate entities or facts from the obsolete passage, and still-current passages can be processed again

#### Scenario: Replace a passage while embedding
- **WHEN** an embedding result arrives after one of its input passages was forgotten or replaced
- **THEN** its vectors are not attached to different passages, and still-current passages can be processed again

#### Scenario: Forget while searching
- **WHEN** a local passage is forgotten or replaced while a search is awaiting another service
- **THEN** the obsolete passage is not returned as a current local result and is not sent to a subsequent reranking call

#### Scenario: Original file remains
- **WHEN** the user confirms Forget for an imported file
- **THEN** its original file remains on disk and the import is excluded from automatic re-ingestion until the user explicitly restores it

#### Scenario: Forget a knowledge note
- **WHEN** the user forgets a note stored in the knowledge database
- **THEN** the note text, indexed content, dependent corrections and text-bearing tombstones are removed from the knowledge database, including its recovery copy, even if the note was never indexed
- **AND** failure to remove any of those records is reported rather than acknowledged as a successful deletion

#### Scenario: Forget a Source
- **WHEN** the user forgets all knowledge from a Source
- **THEN** the Source is disabled until the user enables it again, and its indexed items and Source-dependent corrections are removed
- **AND** forgetting the notes Source also removes its original note text from the knowledge database

#### Scenario: Forget a Thread during indexing
- **WHEN** a Thread is forgotten while its knowledge indexing is in flight
- **THEN** the forget cascade records an exclusion and removes its indexed content and Thread-dependent corrections atomically, so the in-flight operation cannot restore them
- **AND** the cascade reports an error if the recovery copy or knowledge records cannot be removed

#### Scenario: Exclusion cannot remove indexed content
- **WHEN** an exclusion is added but removing one of its matching indexed items or recording its required withdrawal fails
- **THEN** the operation reports a failure instead of acknowledging that the affected knowledge was removed

### Requirement: Retrieval Respects Knowledge State
Knowledge tools and the Knowledge view SHALL apply the same category, Source, revision and permission filters. Current answers SHALL exclude rejected, superseded and unsupported facts, label unresolved disagreements with their evidence, and expose historical facts only when requested as history.

#### Scenario: Unresolved deadline
- **WHEN** the agent retrieves the project's deadline while a relevant disagreement is unresolved
- **THEN** retrieval includes the alternatives and dispute state rather than a single unlabeled definitive date

#### Scenario: Source access changes before background cleanup
- **WHEN** policy or a user exclusion makes an indexed Source item unavailable before its background records are removed
- **THEN** knowledge tools and the Knowledge view do not return the item's passages, extracted relations or item-linked corrections as current knowledge
- **AND** an entity known only from inaccessible knowledge does not reveal its name or other profile metadata through entity lookup

#### Scenario: Direct note reads after access changes
- **WHEN** the notes Source is disabled or an individual note is excluded before cleanup
- **THEN** the Knowledge note list and direct note reader do not expose that note, including when it has not yet been indexed

#### Scenario: Mail draft after access changes
- **WHEN** a mail item is excluded or the mail Source is disabled by policy before cleanup
- **THEN** a mail-to-note draft requested from that indexed mail item is unavailable

#### Scenario: Source counts after access changes
- **WHEN** an indexed item is excluded or file-read permission is revoked before background cleanup
- **THEN** its Source item, passage and pending-work counts no longer include that inaccessible item, while still counting other visible items

#### Scenario: Hidden items precede visible items
- **WHEN** the newest indexed items are inaccessible but older items remain visible
- **THEN** the Source list fills its requested page with accessible items instead of stopping at the inaccessible records

#### Scenario: Many individually excluded Source items
- **WHEN** a large collection has many individually excluded items before its visible items
- **THEN** the Source list can skip those exclusions in bounded batches without repeatedly loading the complete exclusion list for every hidden item
- **AND** it checks current policy and permissions before returning visible items

#### Scenario: Exclusion changes during Source listing
- **WHEN** an item becomes excluded while a Source list is being assembled
- **THEN** the list checks the changed exclusion before returning and fills the page with later visible items when available

#### Scenario: Hidden search candidates precede visible candidates
- **WHEN** inaccessible passages precede accessible passages among the retrieved search candidates
- **THEN** inaccessible passages do not consume the local reranking candidate limit or prevent the accessible candidates from being returned

#### Scenario: Hidden item matches precede visible matches
- **WHEN** inaccessible items appear before accessible items in an item-name lookup
- **THEN** the lookup continues through bounded pages to return accessible matches up to its requested limit

#### Scenario: Hidden entity matches precede visible matches
- **WHEN** inaccessible entities appear before accessible entities in an entity-name lookup
- **THEN** the Knowledge view and knowledge tools continue through bounded pages to find accessible matches rather than reporting no known entity

#### Scenario: Accessible entity evidence follows many inaccessible sources
- **WHEN** an entity has accessible evidence after more than a thousand inaccessible evidence records
- **THEN** entity visibility checks continue through bounded pages and recognize the accessible evidence

#### Scenario: An alias appears only in an inaccessible source
- **WHEN** a visible entity has a noncanonical alias mentioned only in a now-inaccessible item
- **THEN** entity profiles and name lookup do not expose or match that alias, and it is not used as a graph-search seed, until a permitted item mentions it

#### Scenario: A graph connection comes from an inaccessible source
- **WHEN** a relation connecting otherwise visible entities is supported only by an inaccessible item
- **THEN** graph-assisted search does not traverse that relation to find other passages

#### Scenario: Large note collection
- **WHEN** the user browses or searches a large collection of notes
- **THEN** the Source list reads notes in bounded batches and fills its page with matching visible notes without loading every note's contents at once

### Requirement: Category Publication Eligibility
The user SHALL be able to select knowledge from a category for publication only when every required evidence Source kind is permitted by policy.knowledge.sharing.derived_sources (list of Source kinds; default files and notes). Unsupported, disputed or pending facts are excluded. The destination must be a Group allowed by policy.knowledge.sharing.groups and control-plane/team-knowledge.

#### Scenario: Mixed private evidence
- **WHEN** a fact depends on both a document and mail while derived_sources permits only files and notes
- **THEN** the fact is excluded from the publication with the mail restriction explained, including when it also belongs to another category

### Requirement: Category Publication Preview
Category publication SHALL require a preview of the exact selected facts, selected supporting excerpts, public titles, recipient Groups, excluded items and update behavior. Private paths, unrelated excerpts and private graph metadata MUST NOT be uploaded. The user can remove entries or cancel. Copying mail, Chat or transcript-derived content SHALL be stated explicitly before confirmation.

#### Scenario: Share Project Falcon with a Group
- **WHEN** the user selects Project Falcon and engineering as the audience
- **THEN** the preview shows eligible entries and exclusions, the exact text and evidence recipients will see, and publishes only after confirmation

#### Scenario: Company destination
- **WHEN** the control plane exposes a Company Group that the user is permitted to publish to
- **THEN** the picker shows Company with the underlying Group and the breadth of that audience before confirmation

### Requirement: Publication Change Control
A category publication SHALL be a reviewed version of selected knowledge. New facts, broadened audiences, changed statements and added excerpts wait for review. Removed, excluded, rejected, newly disputed or unsupported entries are withdrawn automatically from their publications; unaffected entries remain. The owner can inspect the exact published version and every pending difference.

#### Scenario: Revised deadline was published
- **WHEN** a Source update invalidates a published deadline
- **THEN** withdrawal of that entry is queued immediately, the new deadline waits for review, and unrelated published entries remain unchanged

### Requirement: Honest Sharing Synchronization
Sharing SHALL distinguish private, published, pending update, pending withdrawal, failed and removed-by-admin states. Offline unsharing or forgetting SHALL explain that server removal is pending until acknowledged and retry durably on reconnect. Administrator removal MUST NOT be undone by a retry or an automatic sharing rule.

#### Scenario: Forget while offline
- **WHEN** an owner forgets a Source while disconnected and a publication depends on it
- **THEN** local knowledge is removed immediately and Sharing says that the previously published text may remain available until removal is acknowledged

#### Scenario: Forget an existing shared item offline
- **WHEN** the owner forgets a shared document or deletes a shared note while the control plane is unavailable
- **THEN** the local deletion and an encrypted pending withdrawal are committed together, with the item's title and content-derived metadata removed from the withdrawal record
- **AND** a later synchronization retries the withdrawal rather than treating the missing connection as confirmation
- **AND** the Knowledge view warns that previously shared content may remain available until the withdrawal is confirmed

#### Scenario: Legacy withdrawal is absent for the current username
- **WHEN** an existing item withdrawal is pending and the legacy server returns not found or omits it from the current username's item list
- **THEN** the client retains the pending withdrawal because that response does not establish absence under the original owner
- **AND** the client retries without automatically republishing the item and refuses another share of that item until removal is acknowledged

#### Scenario: Previously shared legacy item is absent from the list
- **WHEN** an item previously reported as shared is absent from a legacy list scoped by the current username
- **THEN** the client says the sharing state cannot be verified, retains its withdrawal identity, and does not claim administrator removal or automatically upload it again
- **AND** Delete all continues to treat that item as requiring verified removal

#### Scenario: Earlier client inferred administrator removal from absence
- **WHEN** an existing local share record was previously marked removed solely because it was missing from a legacy username-scoped list
- **THEN** the client treats its status as unverified and retains its remote identity for item forgetting, folder unsharing and Delete all

#### Scenario: Earlier folder-item withdrawal lacks acknowledgment evidence
- **WHEN** an earlier client recorded an individual folder-item withdrawal without distinguishing a successful delete from a not-found response
- **THEN** the client treats that retained record as unverified and does not discard its remote identity as confirmed removed

#### Scenario: Retry an individual folder-item withdrawal
- **WHEN** an individual item from an automatically shared folder is unshared while offline and its removal is later acknowledged
- **THEN** the item remains excluded from that folder's sharing rule until explicitly shared again

#### Scenario: Cannot record an individual withdrawal
- **WHEN** the client cannot durably record an individual item's pending withdrawal
- **THEN** it reports the storage failure without attempting the remote removal or claiming the item is unshared

#### Scenario: Cannot record a folder withdrawal
- **WHEN** the client cannot record the removal of a shared folder's rule and its pending item withdrawals together
- **THEN** it leaves the prior rule and item records intact, reports the storage failure and attempts no remote removals

#### Scenario: Pending withdrawal status refreshes overlap
- **WHEN** an older sharing-status response arrives after a newer response
- **THEN** the Knowledge view retains the newer withdrawal status

### Requirement: Bounded Knowledge Analysis
Categorization and consistency checking SHALL share policy.knowledge.extraction_daily_calls with extraction and obey policy.knowledge.max_threads and client/desktop-host deferral. Reprocessing SHALL prioritize changed knowledge, persist progress and leave search usable when model access, budget or background execution is unavailable.

#### Scenario: Analysis budget exhausted
- **WHEN** the extraction budget is exhausted during consistency checking
- **THEN** unfinished checks remain visibly queued, manual review and full-text search work, and progress resumes without duplicating completed work
