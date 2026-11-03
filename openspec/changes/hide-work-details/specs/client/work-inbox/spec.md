# Spec Delta

## MODIFIED Requirements

### Requirement: Streaming Thread Feed
The Thread feed SHALL render model output as it streams. Each tool call has a collapsible card showing the tool id, its risk class, its arguments and its result or error, and reasoning content, when provided, has a collapsed block. These cards and blocks are shown only as Work Details Visibility allows.

#### Scenario: Tool call display
- **WHEN** the agent calls files.read during a Run and work details are shown for that Run
- **THEN** the feed shows a collapsed card labelled with the tool id, which expands to show arguments and result

## ADDED Requirements

### Requirement: Work Details Visibility
By default the Thread feed SHALL show only messages, answers, Action items, errors, usage, and the images and cited sources a Run produced; reasoning and tool-call cards are hidden. While a Run is active, one status line names its current step in plain words. A Run no longer active (finished, or waiting for the user) offers Show details, which reveals its reasoning and tool-call cards. The General setting Show work details (default off) shows them for every Run.

#### Scenario: Default feed
- **WHEN** a Run reads two files, reasons, and answers, with Show work details off
- **THEN** the feed shows the user's message and the answer, no reasoning text and no tool cards, and while the Run was active it showed a status line such as "Reading files"

#### Scenario: Details for one Run
- **WHEN** the user chooses Show details on a finished Run
- **THEN** that Run's reasoning block and tool-call cards appear, and other Runs stay compact

#### Scenario: Setting on
- **WHEN** the user turns on Show work details in Settings > General
- **THEN** every Run shows its reasoning blocks and tool-call cards
