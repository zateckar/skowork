# Spec Delta

## ADDED Requirements

### Requirement: Action Preparation Presentation
The summary of an automatic action preparation SHALL be presented with the action_prep Content template (client/work-inbox Content Templates): what is asked, by whom and by when as key facts, and what the user must supply. The Question card and the response step SHALL be unchanged.

#### Scenario: Prepared action item
- **WHEN** an action item from mail is prepared
- **THEN** the Thread shows the key facts and summary as an action_prep surface, followed by the Question card
