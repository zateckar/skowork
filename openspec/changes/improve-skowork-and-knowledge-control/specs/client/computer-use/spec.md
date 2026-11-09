# Spec Delta

## ADDED Requirements

### Requirement: Computer Tool Identities
The client SHALL identify the computer-use Tools as `computer.observe`, `computer.launch`, `computer.screenshot`, `computer.click`, `computer.type`, `computer.keys`, `computer.scroll`, `computer.drag` and `computer.select`. Their availability, risk and execution remain governed by the other requirements of client/computer-use.

#### Scenario: Computer mode disabled
- **WHEN** computer mode is disabled
- **THEN** the administrative policy catalog can identify all nine Tools without offering them to the Run or implying that they are allowed
