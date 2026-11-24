# Spec Delta

## ADDED Requirements

### Requirement: Compaction After The Final Answer
When a top-level Run has received its final answer, the runtime SHALL estimate the Thread's input as its next request would be: system instructions, summary, conversation and the Run's current tool definitions, calibrated as for a request. When that estimate is at or above the compaction trigger, the runtime SHALL compact the Thread before the Run ends, with the same procedure, target and Activity log record as a compaction before a request. A failed compaction at that point SHALL NOT change the answer or the Run's outcome. Child Runs are not compacted after their final answer.

#### Scenario: Final answer carries the Thread over the trigger
- **WHEN** a Run's last request is below the compaction trigger and its final answer brings the Thread's estimated input to or above the trigger
- **THEN** the Thread is compacted before the Run ends as succeeded, the compaction record is automatic and names that Run, and the Thread's context usage is below the trigger afterwards

#### Scenario: Thread stays below the trigger
- **WHEN** a Run ends with the Thread's estimated input below the compaction trigger
- **THEN** no compaction runs and no summarizer request is made

#### Scenario: Compaction after the answer fails
- **WHEN** the summarizer is unavailable while the runtime compacts after a final answer
- **THEN** the Run ends as succeeded with its answer, the Activity log records that compaction failed, and the next request checks the trigger again
