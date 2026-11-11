# Spec Delta

## ADDED Requirements

### Requirement: Publication Capability Discovery
The authenticated knowledge API SHALL advertise its supported publication read and mutation schema versions independently from ordinary item sharing. A missing discovery endpoint or an empty version list means that publication operation is unavailable. The server MUST NOT advertise a version before its authorization, withdrawal and compatibility requirements are implemented.

#### Scenario: Publication support unavailable
- **WHEN** a client connects to a server with no supported publication mutation version
- **THEN** it does not submit a publication mutation and ordinary document and note sharing remain available

#### Scenario: Unsupported mutation version
- **WHEN** a client submits a publication mutation using a schema version the server has not advertised
- **THEN** the server refuses the mutation without changing any publication or ordinary item

### Requirement: Versioned Knowledge Publications
The service SHALL accept an owner's confirmed publication as a stable publication id, expected revision, public title, selected entries, selected evidence excerpts, permitted Source kinds and recipient Groups. A publication SHALL contain only its reviewed content and use the authorization, size and owner-quota limits of Shared Item Publication.

#### Scenario: Publish selected knowledge
- **WHEN** an authorized owner confirms five entries for engineering
- **THEN** the service stores one publication revision containing exactly those five entries and the confirmed excerpts, with no other private category contents

#### Scenario: Source kind forbidden
- **WHEN** a publication declares mail-derived content but policy.knowledge.sharing.derived_sources excludes mail
- **THEN** publication is rejected and the previous revision remains unchanged

### Requirement: Publication Authorization
Every publication write, read, search and withdrawal SHALL enforce the caller's current authorization under control-plane/identity and the owner's applicable sharing policy. Readers see only Groups they belong to; only the owner can publish or withdraw, and authorized administrators can remove with a reason. Remembered client permissions are not server authority.

#### Scenario: Group access removed
- **WHEN** the control plane removes a reader's engineering membership
- **THEN** subsequent search and direct-read requests cannot expose engineering-only publications, excerpts, titles or result counts

#### Scenario: Owner loses sharing permission
- **WHEN** the owner's effective policy no longer permits engineering
- **THEN** that owner's publications cease being returned to engineering without waiting for an owner upload

### Requirement: Stable Publication Ownership
Publication ownership SHALL use the authenticated identity-provider issuer and stable subject, not a mutable username. Existing item shares whose historical subject cannot be established remain explicitly legacy and MUST NOT be silently attributed to a subject solely because a current username matches.

#### Scenario: Owner username changes
- **WHEN** the same authenticated issuer and subject returns with a different username
- **THEN** the owner can withdraw their subject-bound publication without exposing it to a different subject that later uses the former username

#### Scenario: Historical ownership is ambiguous
- **WHEN** a legacy item has only a historical username and the service cannot prove its original issuer and subject
- **THEN** it does not claim that the item is absent for a subject-bound withdrawal or attach it to a new subject based only on that username

### Requirement: Shared Item Ownership
A new or replaced existing item share SHALL be owned by the uploader's issuer and stable subject. Replacing a legacy share of the same username and item id removes the legacy content in the same transaction rather than attributing it. Withdrawal reports confirmed absence only when neither a share of that subject nor any legacy share with that item id exists; otherwise it reports not found without proving absence.

#### Scenario: Owner withdraws after a username change
- **WHEN** the owner of a subject-bound item share signs in with a new username and withdraws the item
- **THEN** the item is removed, and while the owner was away, a different subject using the former username could neither see the owner's Group snapshot applied nor remove the item

#### Scenario: Absence confirmed for a subject
- **WHEN** a subject withdraws an item id that no subject-bound or legacy share holds
- **THEN** the service reports confirmed absence, so a pending client withdrawal for that subject can be completed

#### Scenario: Legacy share under another username
- **WHEN** a legacy share with the requested item id exists only under a different username
- **THEN** the service reports not found without confirming absence and does not remove that share

### Requirement: Company Sharing Destination
The service SHALL optionally designate an existing Group as Company through administrator-controlled server configuration, default unset. The designation is a display alias, not an authorization bypass. The destination is offered only when ordinary membership and policy.knowledge.sharing.groups permit publication to that Group.

#### Scenario: Company not configured
- **WHEN** no Company Group has been designated
- **THEN** the sharing picker offers permitted Groups without inventing a company-wide recipient

#### Scenario: Company is broader than the owner's access
- **WHEN** a configured Company Group is not permitted for the owner
- **THEN** publishing to it is refused with the same authorization checks as any other Group

### Requirement: Atomic Publication Revision
Publishing or changing an audience SHALL replace a publication revision atomically after an expected-revision check, with idempotent retry. Concurrent changes SHALL return a conflict requiring a fresh preview. Readers MUST NOT see mixed entries, evidence or permissions from different revisions.

#### Scenario: Two devices edit a publication
- **WHEN** one device publishes revision 4 and another submits an update based on revision 3
- **THEN** the stale update is refused without overwriting revision 4 or changing its audience

### Requirement: Publication Withdrawal Fencing
The service SHALL remove withdrawn entries and their searchable text, chunks, embeddings and excerpts, preserving unaffected entries. Withdrawal and administrator removal SHALL create an ordering barrier so that delayed uploads cannot restore the removed content. Republishing after user withdrawal requires a fresh explicit confirmation; administrator removal remains blocked until explicitly resolved by an administrator.

#### Scenario: Late retry after removal
- **WHEN** an old publish request arrives after the publication was removed by an administrator
- **THEN** it is refused and the content does not reappear in search

#### Scenario: Partial withdrawal
- **WHEN** the owner withdraws one invalidated entry from a ten-entry publication
- **THEN** new reads and searches expose only the nine surviving entries and no excerpt or vector of the withdrawn entry remains usable

#### Scenario: Last entry withdrawn
- **WHEN** the last remaining entry in a publication is withdrawn
- **THEN** its public title, audience, excerpts and searchable content are removed while the revision barrier remains

### Requirement: Publication Search Provenance
Search results for publications SHALL include owner, permitted audience, publication revision, entry identity, last update and approved evidence, with untrusted provenance under client/agent-runtime. Ranking and returned metadata MUST respect the same authorization and withdrawal checks as direct reads.

#### Scenario: Cite shared knowledge
- **WHEN** an authorized colleague retrieves a published project date
- **THEN** the result identifies its owner and publication revision and opens only the published evidence, not the owner's private source path

### Requirement: Publication Inspection And Audit
Owners and authorized administrators SHALL be able to inspect published revisions and pending removal outcomes. Publication, audience changes, withdrawals and administrator removals SHALL be audited using identifiers, revision, actor, audience and outcome without recording private source content or evidence text in the audit log.

#### Scenario: Admin removes shared knowledge
- **WHEN** a platform-admin removes a publication with a reason
- **THEN** new searches omit it, the owner sees its removed state and reason, and audit records the action without copying its knowledge text

### Requirement: Sharing Compatibility
Existing document and note shares SHALL remain readable with their current identities and explicit update behavior. Clients lacking publication support SHALL receive publication passages only through a compatible read contract and MUST NOT overwrite or silently reinterpret a publication as an ordinary item share.

#### Scenario: Older client
- **WHEN** an older client searches team knowledge
- **THEN** it can receive compatible text passages with citations, while incompatible publication editing is refused without altering the publication

### Requirement: Shared Item Search Rechecks
Before returning an existing shared-item search after asynchronous embedding, the service SHALL recheck its current stored visibility and the reader's current control-plane Group assignments, revocation and token expiry, and omit deleted, withdrawn or newly forbidden items. This recheck supplements rather than replaces the current-identity and owner-policy checks required for publications.

#### Scenario: Shared item removed during search
- **WHEN** an item is withdrawn or its owner is revoked while a search is waiting for an embedding
- **THEN** that search returns no passage, title or audience from the item and does not fail because its chunks disappeared

#### Scenario: Reader Group removed during search
- **WHEN** the reader's control-plane Group assignment is removed while the search waits for an embedding
- **THEN** the resumed search does not return items available only to that Group

#### Scenario: Reader revoked during search
- **WHEN** the reader is revoked or their authenticated token expires while the search waits for an embedding
- **THEN** the resumed search is refused without returning passages

### Requirement: Shared Item Update Rechecks
An asynchronous update of an existing shared item SHALL fail without writing if that item or its audience changed, or its row was withdrawn or recreated, while its text was being embedded. The service SHALL recheck control-plane Group assignments, the owner's policy, revocation, token expiry and current quota before committing a shared-item write; authoritative IdP role freshness and publication revision fencing remain separate requirements.

#### Scenario: Shared item withdrawn during update
- **WHEN** the owner withdraws an existing item while its update is waiting for an embedding
- **THEN** the update is refused and neither the item nor its text reappears

#### Scenario: Another device changes the shared item
- **WHEN** a second update changes the item or its audience while an earlier update waits for an embedding
- **THEN** the earlier update is refused rather than overwriting that change

#### Scenario: Owner membership snapshot changes
- **WHEN** a later request refreshes the owner's stored Group or permission snapshot during an earlier in-flight update
- **THEN** the earlier update does not overwrite the newer snapshot and is refused if it differs

#### Scenario: Owner Group removed during update
- **WHEN** the owner's control-plane Group assignment is removed while an update waits for an embedding
- **THEN** the update is refused and the owner snapshot is refreshed without that Group

### Requirement: Shared Item Content Verification
The service SHALL determine whether item text changed using a server-computed digest of the received text, not a digest supplied by the client. Existing client-visible content-hash fields remain compatible. A pre-upgrade item without a server digest is reprocessed on its next upload.

#### Scenario: Client reuses a content hash for different text
- **WHEN** an existing shared item is updated with different text but the same client-supplied content hash
- **THEN** its searchable content is replaced with the new text rather than retaining the old passages
