# Design

## Context

`extraction_request()` in `client/src/knowledge/knowledge_service.cpp` sends batches of 8 passages as `[i] (date)\n<text>` to the `small` role at temperature 0 and asks for entities and snake_case relations with a confidence and validity dates. `parse_extraction()` (`core.cpp`) does structural cleanup only; `store_extraction()` stores every relation that resolves to known entities, skipping only tombstoned facts. Confidence is stored but never compared. Prioritisation is limited to queue order and a daily call budget (policy.knowledge.extraction_daily_calls, default 2,000). Retrieval fuses full-text, embedding and graph-expansion results and reranks them.

## Goals / Non-Goals

**Goals:** fewer, more meaningful facts; context-rich fact statements; source standing reflected in ranking; user control; no extra model calls per passage.

**Non-Goals:** a perfect notion of importance; organization-wide ranking rules; deleting source items.

## Decisions

### D1. Source context in the extraction request

Each passage header carries structured, client-derived metadata (never taken from the passage text): Source kind; author display name and whether the author is the user; addressing breadth (`direct` for one recipient or a direct Chat, `group` for 2–10 recipients, `broadcast` for more or a distribution list); item title or subject; for files the folder name; for meetings the meeting title and attendee count. The system instruction explains that metadata is trusted context and the passage remains data.

Alternatives: infer standing from the text alone (rejected: the model cannot tell a forwarded memo from a chat); a fixed per-Source weight only (rejected: a mail can be a memo or small talk).

### D2. Salience and durability in the same call

Each relation in the extraction response gets `salience` (0..1: how useful the fact is for the user's future work) and `durability` (`lasting`: roles, ownership, decisions, project facts; `temporary`: plans and states that will soon change; `trivial`: small talk, logistics without consequence). The instruction gives anchors: a decision recorded in a memo or meeting is high; who works on a project is high; a lunch arrangement or a thank-you is trivial. No additional model call is made; the response grows by two fields per relation.

The client drops `trivial` relations and relations with salience below policy.knowledge.min_salience (0.0–0.9, default 0.35). Dropped candidates are counted per Source in Knowledge status (without storing content), so the user can see that filtering happens. Entities that end up with no stored relation and no other reference are not created.

Alternatives: a separate triage call after extraction (rejected: doubles calls within the daily budget); keep everything and only rank (rejected: the graph and the Knowledge view stay cluttered, and inconsistency checks compare more noise).

### D3. Self-contained fact statements

Each stored relation gets a `statement` of at most 200 characters in the passage language, readable on its own ("Petr Novák leads the battery-cost project in Mladá Boleslav since October 2026"), alongside the triple. The statement is what the Knowledge view and knowledge.search show; the exact excerpt required by Evidence For Every Fact remains the evidence. A statement that is longer is truncated at a word boundary; an empty statement falls back to the rendered triple.

Alternatives: show the excerpt only (rejected: excerpts are often partial sentences); longer summaries (rejected: storage and prompt size).

### D4. Temporary facts

A `temporary` relation without `valid_to` gets `valid_to` = passage date + 30 days. After that it is treated as historical by Retrieval Respects Knowledge State: excluded from current answers, available as history.

### D5. Salience-weighted retrieval

After fusion and reranking, the final score is multiplied by a weight from salience and source standing: `w = 0.6 + 0.4 * salience` times a breadth factor (`broadcast` from an organization sender 1.15, `group` 1.0, `direct` 1.0, Chat 0.9). User-marked important facts get salience 1.0; unimportant ones 0.1 (still retrievable by explicit query). The weights are design constants, tuned in spike 1.2, not policy.

Alternatives: hard filtering at query time (rejected: loses recall for explicit questions); passing salience to the reranker (rejected: reranker input is text only).

### D6. User control

The Knowledge view shows importance as three levels (high, normal, low), never as a number or probability, consistent with Honest Knowledge States. The user can mark a fact important or unimportant; the mark is stored with the fact, survives re-extraction of the same evidence and is part of the fact's history. Marking unimportant does not delete the fact; Forget remains the deletion path.

### D7. Existing facts

Facts stored before this change have no salience. They are treated as 0.5 and `lasting` until scored. A background scoring pass sends existing statements in batches of 40 (relation, excerpt, source metadata) to the `small` role, using at most 20% of policy.knowledge.extraction_daily_calls per day, newest Sources first. Scoring never deletes a fact on its own: facts scored below the threshold are hidden from current answers and listed under a "Low importance" filter in the Knowledge view, where the user can restore or forget them.

## Performance

The extraction response grows by roughly 60 tokens per relation (two fields plus the statement); with a typical 10 relations per batch that is about 600 output tokens per call, under 1 s at the gateway's observed speed. Storage grows by about 250 bytes per relation; dropping trivial facts is expected to reduce the relation count (spike 1.1 measures by how much). Retrieval adds one multiplication per result. No change to idle memory.

## Risks / Trade-offs

- [Useful facts are dropped as trivial] -> Conservative default threshold, counts of dropped candidates in Knowledge status, labelled corpus with a recall target for important facts of at least 0.9 before release, policy can lower the threshold.
- [The model rates by tone rather than substance] -> Client-derived source metadata, explicit anchors in the instruction, corpus covering formal small talk and informal decisions.
- [Broadcast boost promotes spam or newsletters] -> Boost only broadcasts from senders inside the organization domain; newsletters are scored by the model and may be trivial.
- [Backfill consumes the extraction budget] -> 20% daily cap, newest first, never deletes.
- [Statements contain errors not in the excerpt] -> The excerpt stays the evidence and is shown next to the statement; Possible Inconsistency Review applies unchanged.

## Migration Plan

Additive columns for salience, durability, statement, user importance and a scored flag. Old facts read as unscored (D7). Downgrade leaves the new columns unused; the older client ignores them.

## Implementation decisions (2026-11-17)

These fill gaps found while implementing tasks 2.1 to 4.1. They refine D1 to D7 and do not change the requirements.

- **Schema (schema version 15).** `relations` gains `salience`, `durability`, `statement` and `scored`; `items` gains `context` (the Source context as JSON, written at indexing); two tables hold user importance marks and the per-day count of dropped candidates by Source. Marks are keyed by the normalised names of the relation's entities and predicate, so they survive re-extraction and a re-created relation takes the mark's salience.
- **Organization sender (D5).** "Organization domain" is the domain of the signed-in user's address. A public-mail domain counts as an organization in that case too, so this boost is only as strict as the user's own address.
- **Entities (D2).** An entity is created when a kept relation names it or when a passage mentions it. A mention counts as a reference; an entity named only by dropped relations is not created.
- **Statement length (D3).** The 200 limit is applied in bytes, so a statement is at most 200 characters. Statements are cut at a word boundary.
- **Passage header (D1).** Each passage is a `<passage>` element whose attributes the client writes. Passage text that would open or close a passage tag is neutralised, so text cannot forge a header.
- **Retrieval weights (D5).** A passage takes the highest salience among the relations extracted from it; a passage with none takes the default 0.5 (w = 0.8). A negative score is divided by its weight, so a higher weight always moves a result up. Team passages keep their order.
- **Current answers (D4, D7).** A relation is historical when it is a temporary fact past its end date, or when it is rated below policy.knowledge.min_salience. Only the history view returns historical relations, labelled with the reason.
- **Background rating (D7).** The rating pass sends at most 40 facts a request, newest Sources first. It counts in both `score_calls` (capped at 20% of extraction_daily_calls) and the shared `extract_calls`. A fact the answer leaves out is marked rated at the default, so it is not requested again and again.
- **Low importance list.** The Knowledge view lists, next to All knowledge and Entities, the facts marked unimportant or rated below policy.knowledge.min_salience (knowledge.facts with importance low). A fact there opens in the reader, where Mark important restores it to current answers and Forget deletes it with a tombstone like any fact deletion. Retrieval still returns these facts only with the history flag.
