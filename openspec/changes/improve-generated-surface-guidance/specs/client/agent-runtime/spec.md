# Spec Delta

## MODIFIED Requirements

### Requirement: Generated Surface Instructions
The runtime SHALL tell the model the supported versioned surface contract and component catalog for Thread answers, including the requirement to retain a readable answer when no surface is usable. The instructions SHALL also say what the interactive components return: a button in an answer sends its label and each control's label and value as the user's next message, and the single button of an `ask_user` form returns them as the answer. They SHALL ask the model to use an `ask_user` form when it needs several structured choices from the user.

#### Scenario: Model composes a response
- **WHEN** the runtime prepares a model request for a Thread with surface rendering available
- **THEN** the model receives the supported surface contract and can choose a suitable composition for the response

#### Scenario: Model needs the user's choices
- **WHEN** the runtime prepares a model request for a Thread with surface rendering available
- **THEN** the instructions and the `ask_user` tool describe the form, what its button returns, and when to use it instead of questions in prose

#### Scenario: Optional follow-up work
- **WHEN** the model could offer further optional work after answering
- **THEN** the instructions tell it to give the complete answer and mention next steps in prose rather than ask
