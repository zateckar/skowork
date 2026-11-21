# Proposal

## Why

Salience triage (add-knowledge-salience) asks the model to rate every fact. In practice it keeps nearly everything. On the test machine on 2026-11-21, 8 Sources (5 Threads, 3 meeting captures) gave 282 stored facts with an average salience of 0.62. Only 4 candidates were dropped. Many of the facts that were kept do not help anyone later:

- **The agent's own work log.** Examples: "Skowork edits index.html in the app's public folder to add two missing element IDs", "Skowork plans to set DevTools page overrides", "A folder-access policy is temporarily blocking file-write access". Facts about other AI assistants also slip in, such as "Kimi works on Passkeys Explained App".
- **Statements that only say something was talked about.** Examples: "Capture of Edge discusses DZC" and "The meeting discussed the testing environment". These name a topic but carry no information about it, and each one creates an entity, such as System "DZC", whose only fact is that it was mentioned.
- **Placeholder participants.** "Speaker 1", "Speaker 2" and "Me" are stored as Person entities and turn up in facts as if they were people.
- **Conversation chatter.** Examples: "Pan Dvořák and Pan Ptáček communicate very frequently on Teams", "Speaker 1 and DZCRZKN have already discussed a recurring technical problem twice", and file paths of files the agent generated.
- **Thread text.** A Thread is indexed with its injected "Current time: …" line, which also becomes the Thread's title in the extraction context.

The user sees all of this in the Knowledge view and in search, and the useful facts drown in it.

## What Changes

- **Common-sense rubric.** Extraction gets a "would a colleague still need this in a month" test, concrete examples of facts to keep and facts to leave out, and calibrated salience anchors.
- **Client gates.** The client applies deterministic common-sense gates to every candidate fact. It never stores:
  - facts about Skowork itself;
  - placeholder participants;
  - "discussed / mentioned / talked about" statements;
  - self-references;
  - file paths taken from Threads.
- **Cleanup of existing facts.** When the rules change, stored facts are cleaned up once. Facts the gates reject are removed, unless the user marked them. The remaining model-rated facts are rated again under the new rubric, through the existing background rating.
- **Thread indexing.** Threads are indexed without the injected run-time line, and under the Thread's own title.
- **"Me" is the user.** "Me" in meeting captures, Threads and notes, and the user's own account name, become the signed-in user's Person.
- **Assistants and Local Apps.** Facts from Threads that name an AI assistant (Kimi, ChatGPT and others) or one of the user's Local Apps are not stored.
- **Duplicate names merged.** A background pass, confirmed by the model, merges spelling, transcription and translation variants (Ship / ŠIP / Šip, Archivas / Archibas, Pan Dvořák / Dvořák). The user can undo a merge with Split.
- **Entities without facts unlisted.** The Entities list no longer shows entities that only appear in passages and have no fact, such as "DZC" once its only fact is gone.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `client/knowledge-graph`: common-sense fact triage, cleanup of stored noise, and Thread passages without run context.

## Impact

Roadmap: proposed after entry 51.

Dependencies:
- Depends on `add-knowledge-salience`: Fact Salience Triage, User Control Of Importance and Scoring Existing Facts. That change must be archived first. At archive, rebase on its archived requirements.
- Builds on `add-knowledge-graph`: Entity And Relation Extraction.

Affected areas: extraction requests, triage before storage, the background rating pass, and Thread indexing. No new policy keys. policy.knowledge.min_salience keeps its meaning and default.

Out of scope:
- naming diarized speakers (Speaker 1, Speaker 2);
- a separate LLM review call per fact;
- server-side triage of team knowledge.
