# Design

## Context

add-knowledge-salience makes extraction rate each relation for salience and durability. The client then drops relations rated `trivial` or below policy.knowledge.min_salience (default 0.35).

Observed on the test machine on 2026-11-21:

- 282 relations from 8 items (5 Threads, 3 meeting captures).
- Average salience 0.62; 92 relations rated temporary.
- 4 candidates dropped in total.

The noise falls into five patterns, and each pattern is recognisable without a model:

1. **The agent's work log** (Threads). Facts about Skowork editing files, debugging, planning or being blocked by policy, and facts about other AI assistants.
2. **"Discusses" statements.** About 25 relations such as `Meeting:Capture of Edge discusses System:DZC`. They record only that a topic came up, and they create entities whose only fact is that mention.
3. **Placeholder participants.** `Speaker 1`, `Speaker 2` and `Me` appear in about 30 relations.
4. **Chatter.** "Communicates frequently on Teams", "already discussed twice", and file paths from generated apps.
5. **The injected run-time line.** Thread items are titled "Current time: Friday 20 November 2026, 11:17 …". `knowledge_thread_item()` takes the first line of the first user message, and run-time injection puts that line first.

The extraction instruction currently gives only three anchors: a memo decision is high, project membership is high, lunch is low. It gives no negative examples, so the model rates almost anything with a named subject 0.5 or more.

D:\DEV\chatbot-sqlite, which works well in practice, uses three things:

- extraction rules with explicit good and bad examples ("Never write 'the document' …"; "BAD claims (DO NOT generate): …");
- deterministic gates after the model: a closed predicate vocabulary, self-loops dropped, required fields;
- reversible routing instead of silent loss.

This design ports the first two. The third already exists here as user importance marks and the Low importance list.

## Goals / Non-Goals

**Goals:**
- Stop storing the five noise patterns.
- Clean up what is already stored.
- Keep recall for real work facts: decisions, ownership, architecture, contacts, constraints.
- No extra model calls per passage.

**Non-Goals:**
- Entity deduplication across spellings.
- Speaker naming.
- Hiding mention-only entities.
- A judge call per fact.
- Changing policy.knowledge.min_salience.

## Decisions

### D1. Common-sense rubric in the extraction and rating instructions

Both the extraction instruction and the background rating instruction get the same rubric.

**Keep test.** "Keep a fact only if a colleague joining this work in a month would need it: who owns, leads, decides or is the contact for what; decisions and their reasons; commitments with an owner or a date; how systems are built, connected or constrained; contracts, prices and deadlines."

**Never propose:**
- (a) Anything about Skowork or any AI assistant, model or agent, or about what the assistant did, tried, planned or failed in a Thread: tool use, file edits, errors, permissions, debugging.
- (b) That a meeting, Thread, document or person discussed, mentioned, talked about or asked about something. State the content itself instead, or nothing.
- (c) Facts about unnamed or placeholder participants (Speaker 1, Me, the user).
- (d) Greetings, logistics, who talks to whom how often, task-list steps, file paths or element IDs of files the assistant created.

**Salience anchors:**
- 0.9: a decision, ownership or approval.
- 0.7: a stable architecture fact or a named contact.
- 0.5: useful context.
- 0.3 or less: progress of a conversation or a passing state.

The rubric includes two good and two bad examples taken from the observed data, so the model sees the exact failure modes. The instruction stays within the same call (no new calls). It grows by about 250 input tokens per request.

Alternatives considered:
- A separate triage call per batch. Rejected: it doubles calls inside the daily budget, and the deterministic gates (D2) catch the patterns that the prompt misses.
- Raising the min_salience default. Rejected: the observed ratings show the problem is miscalibration, not the threshold. A higher threshold alone would also cut real facts that were rated 0.5.

### D2. Deterministic gates

`common_sense_reject()` lives in `knowledge/core.cpp` and is pure. It takes the subject, predicate, object and the Source kind of the passage, and returns a reason code, or none when the candidate passes. `store_extraction()` calls it before `triage_relation()`. A rejected candidate is counted with the dropped candidates of its Source. Names are compared after `normalize_name()`, which lower-cases, folds diacritics and collapses whitespace.

| Reason | Rule |
|---|---|
| `assistant` | Subject or object is `skowork`, `skowork assistant` or `assistant`. |
| `placeholder` | Subject or object matches `(speaker\|mluvci\|mluvčí\|recnik\|řečník\|sprecher\|participant\|ucastnik\|účastník\|teilnehmer\|unknown\|neznamy\|neznámý\|unbekannt)( \d+)?`, or is exactly one of `me`, `i`, `user`, `the user`, `ja`, `já`, `ich`, `uzivatel`, `uživatel`, `benutzer`. |
| `meta` | The predicate, lower-cased with `_` and spaces unified, starts with `discuss`, `mention`, `talk`, `ask`, `is_discussed`, `was_discussed`, `notes`, or equals `relates_to`, `related_to`, `refers_to`, `is_about`, `about`, `concerns`, `covers`. |
| `self` | Subject and object normalise to the same name. |
| `path` | Source is `threads` and the subject or object contains `/` or `\` or ends in a file extension (`.` followed by 1 to 5 alphanumerics, with no space after the dot). |

Rules that apply to names (`assistant`, `placeholder`, `path` for Threads) also stop an entity from being created from a mention. `common_sense_reject_name()` is the per-name part. A user importance mark wins: a marked candidate skips the gates, as it already skips the salience threshold.

Alternatives considered:
- An allow-list of predicates, as in chatbot-sqlite's closed vocabulary. Rejected for now: Skowork's predicates are free snake_case in several languages, so an allow-list would drop real facts. A short deny-list of meta predicates removes the observed pattern without that risk.
- Matching AI model names such as Kimi, GPT or Claude. Rejected: these names collide with people and products. The prompt rule (D1a) covers them.

### D3. One-time cleanup when the rules change

`kTriageRules = 1` is stored in kg meta as `triage.rules`. At the first extraction step after start, if the stored value is lower, one store transaction does the following:

1. For each relation with a passage and without an importance mark, call `common_sense_reject()` with its names, predicate and item Source. Rejected relations are deleted with `delete_relation()`, which also removes orphan entities. No tombstone is written: the gates keep the fact from coming back, and a tombstone means "the user deleted this".
2. Relations that remain and have no mark get `scored = 0`. The background rating (add-knowledge-salience D7) rates them again under the rubric of D1, within its 20% share of the daily budget, newest Sources first. A re-rating below the threshold, or `trivial`, moves a fact to the Low importance list; rating never deletes.
3. Write `triage.rules = kTriageRules`.

User facts (`facts_user`) are a separate table and are not touched. User-edited facts are user facts.

The transaction runs under the store mutex. 300 relations take well under 100 ms. The count of removed relations is logged without content.

### D4. Thread passages without run context

`knowledge_thread_item()`:
- strips a first line that starts with the run-time prefix written by `format_run_time()`, through a new `runtime::strip_run_time()` next to it;
- takes the title from the Thread (`RunStore::thread_title()`) when one is set, and otherwise from the first line of the stripped user text.

Because the turns change, the Thread's content hash changes. The next Thread rescan re-indexes every Thread, its passages are replaced (relations cascade with them), and the Thread is extracted again under the new rules.

### D5. The signed-in user instead of "Me"

`self_reference_name()` (core) is true for `me`, `i`, `myself`, `the user`, `user`, `ja`, `já`, `ich`, `uzivatel`, `uživatel`, `benutzer`, `nutzer`. These names are a subset of the placeholders of D2. The speaker labels (`Speaker 1`) stay placeholders.

The user's names come from `KnowledgeEnv::user` (display name, address) and the new `KnowledgeEnv::user_names` (the account principal, which is `DZCRZKN` on the test machine; the model used it as the user's name). The address local part also counts.

`store_extraction()` works on a copy of the extraction. In passages whose Source is `meetings`, `threads` or `notes`, it rewrites a self-reference, or a Person named with one of the user's names, to the user's Person:
- name: the display name, else the principal, else the address;
- email: the address, when there is one.

`resolve_entity()` then links the Person by email (People Resolution), and directory enrichment can fill in the real name. Without any user name, the rewrite does nothing and the gates drop the fact as before.

At the cleanup of D3, before the gates run, every Person entity named with a self-reference or a user name is merged into the user's Person with `merge_entities()`. Their facts survive and the gates no longer match them.

Alternative considered: rejecting "Me" as in D2. Rejected, because it lost decisions the user stated in meetings (5 facts on the test machine).

### D6. Assistants and Local Apps in Threads

For Source `threads`, `common_sense_reject_name()` also returns `assistant` for well-known AI assistant and model names: `kimi`, `chatgpt`, `gpt` with or without a version, `claude`, `gemini`, `copilot`, `llama`, `mistral`, `deepseek`, `qwen`, `grok`, `perplexity`. It returns `local_app` for a name that contains the name of a Local App.

Local App names come from the new `KnowledgeEnv::local_apps`, which gives the app id and its manifest title. Ids are compared with `-` and `_` as spaces, and a trailing "app" is ignored, so "Passkeys Explained App" matches `passkeys-explained`.

These rules apply only to Threads. A meeting may discuss a real person called Kimi, or a product called Gemini; in a Thread, these names almost always mean the assistant's own tooling. What remains of a work log, for example "HTML application has_bug …", is left to the rubric (D1). The real-model check is recorded below.

### D7. Duplicate entity resolution

`resolve_step()` is a background step after `score_step()`. It shares the scoring share of the budget and counts in `score_calls` and `extract_calls`. It runs when entities exist with an id above the kg meta `resolve.after`.

**Candidates.** Each new entity is paired with up to 15 existing entities whose folded names are similar:
- an edit distance of at most 2 for names of up to 8 characters, and at most a quarter of the length for longer names;
- or one name contains the other as a word;
- or they share a word of 4 or more characters, after dropping honorifics (`pan`, `pani`, `paní`, `herr`, `frau`, `mr`, `ms`, `mrs`, `dr`, `ing`).

Different types may pair, except Person with non-Person, because the model often types a system as an Organization (F5). A request holds at most 120 names.

**Question to the model.** Which of these names refer to the same real thing? Spelling, transcription, diacritics, translation, plural and title variants count as the same. A part is never the same as its whole (Ship vs Ship portal, Edge vs Capture of Edge), and test, production and development variants are different things (Test ship vs Production ship). When unsure, the names are left apart. Answer: `{"groups": [[i, j, …]]}`.

**Client checks.**
- Indexes must be valid, and a group needs 2 or more names.
- Person with non-Person is never merged.
- Persons with different emails are never merged.
- An entity whose key marks it as split by the user (`-split:`) is never merged.
- The target is the member with the most relations, then the one with an email, then the lowest id.

Two entities that a stored fact connects are never merged ("Archiving Services separate_from Archivas"). Such a fact says they are two things, and a merge would drop it as a self-reference.

Each merge goes through `merge_entities()`, which keeps every name as an alias. Split undoes it, and later extraction resolves the variant names to the merged entity through the aliases. The pass then sets `resolve.after` to the highest id it covered.

Alternative considered: deterministic merging only. Rejected, because the observed variants cross languages ("Produkční ship" and "Production ship") and transcription ("Jiřa", "Gira", "Giry"), and edit distance alone would merge "Kore" with "Core".

### D8. Entities without facts are not listed

`visible_entities()` adds a condition to the unfiltered list. A lookup by name is unchanged, because Settings merge and split and the `knowledge.entity` tool use it. An entity is listed only when at least one of these exists for it:
- a relation as subject or object;
- a user fact (`facts_user`);
- an email address (a Person known from mail or the directory).

Mentions remain, so passages that name the entity are still found by full-text search, and graph expansion still sees them.

## Performance

| Part | Cost |
|---|---|
| Extraction requests | About 250 extra input tokens each; no extra calls. |
| Gates | A few string comparisons per candidate. |
| Cleanup | One transaction per rules version. |
| Re-rating (observed data) | About 8 calls of 40 facts, inside the existing scoring share. |
| Thread re-indexing | One extraction call per 8 changed passages, inside the extraction budget. |

## Risks / Trade-offs

- **[A real fact uses a meta predicate, such as "the contract covers maintenance"]** → `covers` is rare among real facts in the observed data, and a decision about scope is usually phrased as `includes` or `requires`. The deny-list is short and tested.
- **[A real person is called "Speaker" or similar]** → Only bare labels, optionally numbered, match. "Petr Speaker" does not.
- **[Facts about Skowork the product, for example its owners]** → `Skowork` as a subject or object is rejected. Facts about the product's team can still name the team or project. Accepted, because the agent's self-reports dominate.
- **[The cleanup removes facts a user relied on]** → Marked facts and user facts are never removed. The items and passages stay searchable. Only rule-matched relations are deleted.

## Replay Of Observed Facts (2026-11-21)

The 282 relations exported from the test machine were replayed through `common_sense_reject()`. 60 were rejected (21%) and 222 kept:

| Reason | Count | Examples |
|---|---|---|
| `meta` | 29 | Every "Capture of Edge discusses …", "DZCRZKN discussed_with Supplier", "Variants A and B relates_to Software". |
| `placeholder` | 23 | All Speaker 1, Speaker 2 and Me facts. |
| `assistant` | 7 | All "Skowork edits / debugs / plans / works_on …". |
| `path` | 1 | `passkeys-explained/public/index.html is_file_of …`. |

The first version of the `path` rule also rejected "External Vendor / Microsite". It was narrowed so that a name with spaces needs a backslash to count as a path, and a test now covers that case. No other rejected relation was a useful fact.

Facts from the same agent work log that the gates keep are left to the rubric (D1) and the re-rating (D3): "Kimi works on Passkeys Explained App", "HTML application has_bug Missing element IDs bug", "Local Apps host blocks Passkeys Explained App". With the "Leave out (a)" rule, the re-rating is expected to rate them trivial.

The cost of the `placeholder` rule is that facts the user stated in a meeting as "Me" are lost, for example "Me decided_against Pro 4T". Mapping "Me" to the signed-in user is listed as out of scope and is the natural follow-up.

## Real-Model Checks (2026-11-21)

Both checks used the organization gateway configured in `.env` (`skoda-llm-primary`), with the 282 facts and 214 entities exported from the test machine. The scripts parsed the instructions from the C++ source, so they sent exactly what the client sends.

**Rating (D1, D3).** After the gates, 222 facts remain, and the background rating request was sent for them in batches of 40. Three versions of the wording were tried:

| Wording | Rated low or trivial | Problem |
|---|---|---|
| "fails the keep test → trivial 0.1" | 42 | Hid real architecture facts, such as "Public microsites isolated_from Production database" (0.2). |
| Keep test spelled out | 5 | Kept "HTML application has_bug Missing element IDs bug" at 0.65. |
| Final: the leave-out list first, then the keep test | 5 | None found. |

With the final wording, the 5 low facts are: the two work-log facts (HTML application bug, folder-access policy on Walkthroughs App), "communicates frequently on Teams", "created based on knowledge of", and "Produkční databáze contains Veřejné mikroci". The key decision rates 0.9, and the Archivá modules and F5 routing rate 0.6 to 0.85.

Answers vary between identical runs, so the rubric shapes the outcome but does not guarantee it.

The gateway model is a reasoning model. A request capped at 4,000 output tokens spent all of them on reasoning and returned nothing. The rating request therefore now allows 2,048 output tokens, the same as extraction. Extraction works in production with 2,048, so the client's `small` role does not reason as heavily as this model.

**Resolution (D7).** Persons were sent in one request and the other names in sorted batches of 60 to 70 (a request of all 179 non-person names ran out of tokens). Groups returned:
- Correct: Dvořák/Pan Dvořák, Ptáček/Pan Ptáček, Šep/Pan Šep, Ship/ŠIP, Entra/Entře/Entru/Entry, Gira/Giry/Jiřa, Pro 4T/Pro4T, Production ship/Produkční ship, Ship test/Test ship/Testovací ship, Varianta A/Variant A, Moap team/Moap tým, Web client/Web klient, Ticketing system/tool/Tyketovací nástroj, and the three Passkeys app names.
- Wrong: the first answer added "Archiving Services" and "Archive system" to the Archivas group. Three things address this:
  - the instruction now names these cases;
  - the client never sends such pairs, because its candidate filter does not find "Archivas" and "Archiving Services" similar;
  - the fact veto blocks the merge even if they are sent.
- Judgment calls: Tomáš/Tomáš Angra, Marek/Marka, Facility management/Facility operations. The user can split any of these.

## Deployment On The Test Machine (2026-11-21)

Release builds 0.1.1-dev.30 to dev.34 (`.kbtriage`) were installed over `D:\Apps\SkoWork`. The binaries and databases are backed up in `D:\Apps\SkoWork-backup-20261121`. Three gaps found there were fixed:

- **Ratings never arrived.** With 40 facts per request and a cap of 2,048 output tokens, the gateway model used every token for reasoning and returned nothing. The rating pass then retried every 10 minutes without progress.
  - Measured: 10 facts per request take up to 2,750 output tokens.
  - Now: `kScoreBatch` is 10 and `kKnowledgeOutputTokens` is 8,192. Extraction, rating and resolution all use that cap. Resolution sends at most 60 names.
- **Threads were not indexed again.** The sweep queues only Threads that were never indexed, so the Thread fix (D4) had no effect on existing Threads.
  - Now: the cleanup queues every Thread, and unchanged ones are skipped by their hash. Rules version 2.
- **The model spelled the app differently.** Re-extraction produced "Passkey explainer app", which does not exactly match the app name "Passkeys Explained".
  - Now: the Local App rule allows small spelling differences. Rules version 3.
- **Rubric contradicted D5.** Rubric 1 still listed "Me or the user" among the placeholders to leave out, so the user's own facts were rated 0.1.
  - Now: rubric 2 says that "Me", "I" and the passage author are the user. Rules version 4.
  - Checked against the real model: the 24 former "Me" and DZCRZKN facts rate 0.65 to 0.95, except one weak fact.

Facts are now rated again only when the rubric version changes (`kTriageRubric`, kg meta `triage.rubric`), not with every change of the gate rules.

Results:

| Measure | Before | After |
|---|---|---|
| Facts | 282 | 155 |
| Thread titles | "Current time: …" | The real Thread titles |
| Entities | 214 | 162, of which 87 have facts and appear in the Entities list |

The cleanup removed 61 facts. Thread re-extraction under the new rules replaced the facts of the Thread passages. The resolution pass merged 37 entities. The merges include:
- Ship/ŠIP; Entra/Entře/Entru/Entry; Gira/Giry; Pro 4T/Pro4T; Production/Produkční ship, database and entry; Test ship/Ship test/Testovací ship;
- Pan Šep/Šep, Dvořák/Pan Dvořák, Ptáček/Pan Ptáček;
- the three spellings each of Variant A and Variant B;
- Škoda/Škoda Auto;
- Me into the user's Person.

"External Suppliers" was merged into "External Vendor"; this is a judgment call.

## Migration Plan

No schema change. The kg meta key `triage.rules` drives the one-time cleanup. A downgraded client ignores the key and keeps whatever remains.
