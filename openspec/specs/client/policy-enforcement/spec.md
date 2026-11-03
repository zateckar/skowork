# client/policy-enforcement Specification

## Purpose
How the client enforces the effective policy on every tool call: approval decisions (allow / approve / deny), network egress rules, the rule that untrusted content cannot grant permissions, and what the agent is told when policy withholds a tool.

## Requirements

### Requirement: Withheld Tool Explanation
When the agent's tool search matches withheld tools, the client SHALL add to the result which capability is withheld, the policy key that controls it, and that an administrator can change it. Withheld tools MUST NOT be loaded or callable; a direct call returns the same explanation. A tool is withheld when policy.tools.shell.mode or policy.tools.computer.mode switches it off, or when policy.tools.rules or policy.tools.risk deny every call of it.

#### Scenario: Computer use switched off
- **WHEN** policy.tools.computer.mode is off and the agent searches for tools to click and type in a desktop application
- **THEN** the search result says that computer use is switched off by policy.tools.computer.mode and that an administrator can enable it, and no computer tool is loaded

#### Scenario: Unrelated search
- **WHEN** the agent searches for tools that match no withheld tool
- **THEN** the result contains no withheld-tool explanation

### Requirement: Dictation Policy
The client SHALL apply policy.dictation.mode (off or on, default on, owned by client/dictation). While it is off, the composers show no Dictate button and the dictation RPCs refuse with policy_denied. Switching it off during a recording discards the recording.

#### Scenario: Switched off
- **WHEN** an administrator sets policy.dictation.mode to off for the user's Group
- **THEN** the Dictate button disappears from the composers at the next policy update
