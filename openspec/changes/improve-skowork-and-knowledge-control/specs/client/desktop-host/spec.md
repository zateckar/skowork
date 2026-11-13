# Spec Delta

## ADDED Requirements

### Requirement: Skowork Desktop Identity
The desktop SHALL identify itself as Skowork in window titles, taskbar and tray text, notifications, dialogs, accessibility names, About and executable product metadata. Its primary executable and downloadable desktop package SHALL use the Skowork name.

#### Scenario: Launch the renamed product
- **WHEN** the user launches the distributed desktop package
- **THEN** the executable, main window, tray menu, About view and product metadata identify the app as Skowork


### Requirement: Speech Release Availability
Every published desktop version, including a development version, SHALL have a compatible optional speech package with authenticated metadata and offline import support under client/speech-models. A core-only installation SHALL provide the setup path without bundling the large models into the core package.

#### Scenario: Fresh published package
- **WHEN** the user runs a published desktop zip on a machine without a development checkout
- **THEN** speech setup can acquire its compatible package or import the matching offline package, and does not depend on developer model paths

#### Scenario: Missing speech artifact
- **WHEN** a release lacks a verified compatible speech package or its acquisition metadata
- **THEN** release validation fails instead of publishing a desktop version with an unusable setup action
