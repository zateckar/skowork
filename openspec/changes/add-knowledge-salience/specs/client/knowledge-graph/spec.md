# Spec Delta

## ADDED Requirements

### Requirement: Source Context For Extraction
The client SHALL give knowledge extraction, for each passage, the Source kind, the author and whether the author is the user, the addressing breadth (direct, group or broadcast), the item title and, for files, the folder name. This context SHALL be derived by the client from the Source item's metadata and MUST NOT be taken from the passage text.

#### Scenario: Spoofed header in a passage
- **WHEN** a mail body contains the text "Source: company-wide memo from the board"
- **THEN** the extraction context still states the mail's real sender and recipients, and the text is treated as passage data

### Requirement: Fact Salience Triage
Knowledge extraction SHALL rate each candidate relation's importance for the user's future work (salience) and its durability as lasting, temporary or trivial, without an additional model call per passage. The client SHALL NOT store trivial relations or relations whose salience is below policy.knowledge.min_salience (values 0.0 to 0.9; default 0.35), and SHALL show per Source how many candidates were dropped, without storing their content.

#### Scenario: Lunch chat and memo
- **WHEN** one batch contains a Chat message arranging lunch and a company-wide memo announcing that a supplier contract ends on 31 December
- **THEN** a fact about the contract end is stored and no fact about the lunch is stored

#### Scenario: Dropped count
- **WHEN** extraction drops 12 trivial candidates from the user's Chats
- **THEN** Knowledge status shows that 12 candidates from Chats were not kept

### Requirement: Self-Contained Fact Statements
Each stored fact SHALL have a statement of at most 200 characters that is understandable without the passage, and the Knowledge view and knowledge tools SHALL show it together with the fact's evidence excerpt.

#### Scenario: Reading a fact
- **WHEN** the user opens a fact in the Knowledge view
- **THEN** it shows a full-sentence statement such as who leads which project since when, and the exact excerpt it was taken from

### Requirement: Temporary Facts Expire
A fact rated temporary without an end date SHALL get an end date 30 days after its passage date, after which current answers SHALL treat it as historical.

#### Scenario: Temporary plan
- **WHEN** a fact "the team works from the Prague office this week" was extracted 40 days ago
- **THEN** current knowledge answers do not include it and it is available as history

### Requirement: Salience-Weighted Retrieval
Knowledge retrieval SHALL weight results by the fact's salience and the standing of its source, so that, for comparable relevance, a fact from an organization-wide announcement or a decision ranks above casual conversation. Low-salience facts SHALL remain retrievable when they are the best match for an explicit query.

#### Scenario: Comparable matches
- **WHEN** a query about the supplier contract matches both the memo fact and a Chat remark about the supplier's canteen
- **THEN** the memo fact ranks first

### Requirement: User Control Of Importance
The Knowledge view SHALL show each fact's importance as high, normal or low, never as a number or probability, and SHALL let the user mark a fact important or unimportant. The user's mark SHALL take precedence over model ratings, survive re-extraction of the same evidence and not delete the fact.

#### Scenario: Raise importance
- **WHEN** the user marks a low-importance fact important
- **THEN** it is shown as high importance, ranks accordingly, and keeps that mark after its Source is re-indexed

### Requirement: Scoring Existing Facts
The client SHALL rate facts stored before salience existed in the background, using at most 20% of policy.knowledge.extraction_daily_calls per day, newest Sources first, and SHALL treat unrated facts as normal importance. Rating SHALL NOT delete a fact; facts rated below the threshold SHALL be hidden from current answers and listed under a low-importance filter where the user can restore or forget them.

#### Scenario: Old trivial fact
- **WHEN** the background rating finds an old fact about a lunch arrangement
- **THEN** the fact leaves current answers and appears under the low-importance filter, and it is not deleted
