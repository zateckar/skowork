# Spec Delta

## ADDED Requirements

### Requirement: Citation Numbers Per Run
The number n of each passage that knowledge.search returns SHALL be unique within a Run, so that a number in an answer names exactly one passage of that Run. A later search in the same Run continues the numbering, and a new Run starts again at 1.

#### Scenario: Two searches in one Run
- **WHEN** the agent calls knowledge.search twice in one Run, the first returning 8 passages and the second 5
- **THEN** the first search numbers its passages 1 to 8, and the second numbers its passages 9 to 13

#### Scenario: New Run
- **WHEN** the agent calls knowledge.search in a new Run
- **THEN** its first passage is numbered 1
