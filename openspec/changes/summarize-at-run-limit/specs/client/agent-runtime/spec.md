# Spec Delta

## MODIFIED Requirements

### Requirement: Run Limits
The runtime SHALL enforce per-Run limits from policy.llm.run_limits.attended and policy.llm.run_limits.unattended. Each has model_calls, tool_calls and wall_minutes. Defaults: attended 60, 200 and 60; unattended 30, 80 and 30. When a limit is reached, the Run SHALL execute no further tool calls and end with a final answer (Final Answer At A Limit). Time spent interrupted does not count toward wall_minutes.

#### Scenario: Tool call limit reached
- **WHEN** a Run reaches its maximum number of tool calls
- **THEN** no further tool calls are executed, and the Run ends as Final Answer At A Limit defines

## ADDED Requirements

### Requirement: Final Answer At A Limit
At a limit, the runtime SHALL give each unexecuted tool call a result stating that the limit stopped it, then make one final model call without tool definitions asking for what was done, found and remains. That call does not count toward model_calls and is made even after wall_minutes. The Run ends succeeded with its answer and records the limit as a Run event. If the call fails or gives no answer, the Run fails with a reason naming the limit.

#### Scenario: Answer at the tool call limit
- **WHEN** a Run reaches its maximum number of tool calls while the model has requested two more tool calls
- **THEN** both calls get a result stating that the limit stopped them, one final model call is made without tool definitions, and the Run ends succeeded with that answer and a Run event naming "tool call limit reached"

#### Scenario: Final call fails
- **WHEN** a Run reaches its maximum number of model calls and the final model call fails with MODEL_UNAVAILABLE
- **THEN** the Run fails with reason "model call limit reached"

### Requirement: Child Run At A Limit
When a Child Run ends at a limit (Final Answer At A Limit), the result its parent receives SHALL state that the subagent stopped at that limit, followed by the child's final answer.

#### Scenario: Subagent reaches a limit
- **WHEN** a Child Run reaches its wall_minutes limit
- **THEN** it makes its final model call, ends succeeded, and its parent receives its answer as the result of the call that started it, preceded by a statement that the subagent stopped at its wall time limit
