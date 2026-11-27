# Spec Delta

## ADDED Requirements

### Requirement: Content Template Instructions
The runtime SHALL provide, for each Content template, instructions that name the template, the components it uses and in what order, one valid example, and the rule to write a short prose summary before the block. A producer that presents its content with a Content template SHALL include those instructions in its Run's instructions whether or not the generated surface setting is enabled. The instructions SHALL NOT offer controls.

#### Scenario: Setting off
- **WHEN** the generated surface setting is off and the Daily report Run starts
- **THEN** the model receives the daily_report template instructions and not the general surface instructions for chat answers
