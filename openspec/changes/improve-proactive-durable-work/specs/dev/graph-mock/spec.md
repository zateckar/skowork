# Spec Delta

## ADDED Requirements

### Requirement: Stable Delta Pagination For Concurrent Test Changes
The mock SHALL preserve the response items of each mail and calendar delta sequence across its continuation pages. A change after a sequence begins SHALL be observable in a subsequent delta sequence, rather than shifting an existing continuation offset and silently omitting an unchanged item. This test behavior does not assert stronger guarantees for the real Graph service.

#### Scenario: Delete an earlier item during paging
- **WHEN** a test deletes an item already returned by a paged delta sequence before the next page is read
- **THEN** the remaining pages retain their original unchanged items and the subsequent delta reports the deletion.

#### Scenario: Change an event during paging
- **WHEN** a test modifies a calendar event after a paged sequence begins
- **THEN** paging retains the original sequence and a subsequent delta reports the new revision.
