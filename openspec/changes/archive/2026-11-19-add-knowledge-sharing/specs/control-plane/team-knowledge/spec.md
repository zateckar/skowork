# Spec Delta

## Purpose

Defines the team knowledge space: knowledge items that users share with Groups, how they are stored and searched, and who may see and remove them.

## ADDED Requirements

### Requirement: Shared Item Publication
The control plane SHALL accept a shared item from its owner: a stable item id chosen by the client, title, Source kind (files or notes), the item's text, a content hash and the target Groups. It MUST refuse a target Group the owner is not a member of, or that the owner's effective policy.knowledge.sharing.groups does not permit, with 403. Publishing an existing item id of the same owner replaces its text and Groups. Text larger than the server limit (default 2 MB per item) is refused with 413.

#### Scenario: Share with a permitted Group
- **WHEN** alice, a member of engineering whose policy permits engineering, publishes an item to engineering
- **THEN** the item is stored with alice as owner and engineering as its Group

#### Scenario: Group the owner is not in
- **WHEN** alice publishes an item to sales, which she is not a member of
- **THEN** the request is refused with 403, and nothing is stored

### Requirement: Team Knowledge Search
The control plane SHALL answer a search with passages of items shared with any Group the caller is a member of, ranked by a combination of full-text and semantic similarity. Item text is chunked and embedded through the `embed` role of control-plane/llm-gateway on behalf of the owner, counting against the owner's embedding budget. Each passage carries the item id, title, owner, Group, Source kind and last update. Items of other Groups MUST NOT be returned.

#### Scenario: Member of another Group
- **WHEN** carol, who is not a member of engineering, searches for words of an item shared only with engineering
- **THEN** the item is not returned

### Requirement: Unsharing And Removal
The owner SHALL be able to unshare an item, which deletes its text, chunks and embeddings from the team knowledge space. A platform-admin can remove any shared item, with a reason. When the owner leaves a Group, or policy no longer permits the Group, the item stops being returned for that Group. When the owner is revoked under control-plane/fleet-registry, their shared items stop being returned. Every publish, unshare and removal is recorded in the audit log.

#### Scenario: Admin removes an item
- **WHEN** a platform-admin removes a shared item with the reason "contains customer data"
- **THEN** the item is deleted, the audit log records the admin, owner, item title and reason, and the owner's client shows the item as no longer shared
