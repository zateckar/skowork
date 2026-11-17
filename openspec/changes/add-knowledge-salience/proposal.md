# Proposal

## Why

Knowledge extraction stores every relation it finds, as short triples, with equal weight. A casual Chat about lunch and a company-wide memo produce facts of the same standing. The extractor sees only passage text and a date, not what kind of source it is, who wrote it or how widely it was sent. The stored confidence is not used to filter anything. The result is a large graph of terse, often trivial facts that crowd out the ones that matter in search and in the Knowledge view.

## What Changes

- Give extraction the source context it needs: the Source kind, author, addressing breadth (direct, small group, broadcast), title and, for files, the folder.
- Triage candidate facts before storing: each relation gets an importance (salience) and a durability (lasting, temporary, trivial). Trivial facts and facts below a salience threshold are not stored.
- Store each fact with a short, self-contained statement that keeps the context the triple loses.
- Let temporary facts expire from current answers.
- Weight retrieval by salience and source standing, so a memo outranks small talk.
- Show importance in the Knowledge view and let the user raise or lower it; the user's choice wins and is kept on re-extraction.
- Score existing facts in the background within the extraction budget.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `client/knowledge-graph`: source context in extraction, salience triage, fact statements, temporary facts, salience-weighted retrieval and user control of importance.

## Impact

Roadmap: proposed after entry 47. Depends on `add-knowledge-graph` (Entity And Relation Extraction, Extraction Call Budget, Hybrid Retrieval With Provenance) and `improve-skowork-and-knowledge-control` (Evidence For Every Fact, Honest Knowledge States, Retrieval Respects Knowledge State). Both must be archived before this change; at archive, rebase on their archived requirements. Knowledge extraction, storage, search and the Knowledge view are affected; publication eligibility (add-knowledge-sharing, improve-skowork-and-knowledge-control) is unchanged except that only stored facts can be published. New policy key: policy.knowledge.min_salience (owned by client/knowledge-graph).

Out of scope: organization-wide source ranking managed in the control plane, per-Group salience rules, re-extraction of all history with a larger model, changes to entity types, and team knowledge ranking on the server.
