# Spec Delta

## ADDED Requirements

### Requirement: Common-Sense Fact Triage
The client SHALL NOT store a candidate fact that names Skowork itself or a placeholder participant (a speaker label, "Me", "the user"), that only says something was discussed or mentioned, whose subject and object are the same, or that comes from a Thread and names a file path. Rejected candidates SHALL count as dropped candidates of Fact Salience Triage; candidates the user marked SHALL NOT be rejected.

#### Scenario: Agent work log
- **WHEN** a Thread passage says that Skowork edited index.html to add two missing element IDs
- **THEN** no fact about Skowork or index.html is stored from it, and the candidate is counted as not kept for Threads

#### Scenario: Meeting mentions a system
- **WHEN** a meeting capture yields the candidate "Capture of Edge discusses DZC"
- **THEN** that fact is not stored

#### Scenario: Useful fact kept
- **WHEN** a meeting capture yields "Public microsites must not connect to the production database"
- **THEN** the fact is stored as before

### Requirement: No Entities From Rejected Names
The client SHALL NOT create an entity, from a relation or from a mention, whose name Common-Sense Fact Triage rejects.

#### Scenario: Placeholder speaker
- **WHEN** a transcript yields "Speaker 1 will confirm variant A"
- **THEN** no fact naming "Speaker 1" is stored, and no Person "Speaker 1" is created

### Requirement: Common-Sense Extraction Guidance
The extraction and background rating requests SHALL tell the model to keep only facts a colleague would still need later, and not to propose facts about what the agent did or planned in a Thread, about AI assistants, that something was discussed, or about conversation logistics.

#### Scenario: Instruction content
- **WHEN** the client sends an extraction request
- **THEN** its instruction states the keep test, the kinds of facts to leave out and salience anchors with examples

### Requirement: Cleanup Of Stored Noise
When the client's triage rules change, the client SHALL once, without user action, remove stored facts that Common-Sense Fact Triage would reject and that carry no user importance mark. It SHALL NOT remove user facts, marked facts, items or passages.

#### Scenario: Upgrade with existing noise
- **WHEN** a client with stored facts "Skowork edits index.html" and "Capture of Edge discusses DZC" starts with the new triage rules
- **THEN** both facts are gone from the Knowledge view and search results, and the meeting transcript and Thread are still found by full-text search

#### Scenario: User mark survives cleanup
- **WHEN** the user marked a fact naming "Speaker 1" important before the upgrade
- **THEN** the fact stays and keeps its mark

### Requirement: Re-Rating After Rule Changes
When the client's triage rules change, the client SHALL rate again the remaining model-rated facts without a user mark, through the background rating of Scoring Existing Facts and within its share of policy.knowledge.extraction_daily_calls.

#### Scenario: Re-rated fact
- **WHEN** a stored fact rated 0.5 under the old rules is rated 0.2 under the new rules
- **THEN** it leaves current answers and appears under the low-importance filter, and it is not deleted

### Requirement: The User Instead Of "Me"
In meeting captures, Threads and notes, the client SHALL record a Person named "Me", "I" or "the user" (and their cs and de equivalents), or named with the signed-in user's account name or address, as one Person for the signed-in user. Without a known signed-in user, Common-Sense Fact Triage applies to those names.

#### Scenario: Decision stated as Me
- **WHEN** a meeting capture yields "Me decided against the Pro 4T test environment"
- **THEN** the fact is stored for the signed-in user's Person, not for a Person named "Me"

#### Scenario: Existing Me entity
- **WHEN** the triage rules change and a Person "Me" with facts exists
- **THEN** it is merged into the signed-in user's Person and its facts are kept

### Requirement: Assistants And Local Apps In Threads
The client SHALL NOT store a fact from a Thread that names a well-known AI assistant or model, or a Local App, as its subject or object.

#### Scenario: Another AI assistant
- **WHEN** a Thread yields "Kimi works on Passkeys Explained App"
- **THEN** that fact is not stored

### Requirement: Duplicate Entity Resolution
The client SHALL, in the background and within the rating share of policy.knowledge.extraction_daily_calls, ask the small model which entity names refer to the same thing (spelling, transcription, translation or title variants) and merge each confirmed group into its entity with the most facts, keeping every name as an alias so the user can split it again. It SHALL NOT merge Persons with different email addresses, and SHALL NOT merge again an entity the user split.

#### Scenario: Transcription variants
- **WHEN** entities "Ship", "ŠIP" and "Šip" name the same system
- **THEN** they become one entity with all three names, and its facts include the facts of all three

#### Scenario: Different things stay apart
- **WHEN** entities "Ship" and "Ship portal" exist
- **THEN** they are not merged

### Requirement: Entities Without Facts Are Not Listed
The Entities list, when not filtered by a name, SHALL NOT show an entity that has no stored fact, user fact or email address. Such an entity SHALL still be found by its name and SHALL stay linked to the passages that mention it.

#### Scenario: Only mentioned
- **WHEN** the only fact about System "DZC" was removed and a meeting passage still mentions it
- **THEN** "DZC" is not in the Entities list, a lookup of "DZC" by name still finds it, and the meeting passage is still found by searching for "DZC"

### Requirement: Thread Passages Without Run Context
When the client indexes a Thread for knowledge, it SHALL leave out the run context the client adds to messages, such as the current-time line, and SHALL title the item with the Thread's title or, when there is none, the first line of the user's text.

#### Scenario: Thread title
- **WHEN** a Thread whose first message starts with the current-time line is indexed
- **THEN** its knowledge item is not titled with the current time, and no passage contains the current-time line
