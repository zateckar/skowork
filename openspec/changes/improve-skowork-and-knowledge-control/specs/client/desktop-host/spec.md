# Spec Delta

## ADDED Requirements

### Requirement: SkoWork Desktop Identity
The desktop SHALL identify itself as SkoWork in window titles, taskbar and tray text, notifications, dialogs, accessibility names, About and executable product metadata. Its primary executable and downloadable desktop package SHALL use the SkoWork name.

#### Scenario: Launch the renamed product
- **WHEN** the user launches the distributed desktop package
- **THEN** the executable, main window, tray menu, About view and product metadata identify the app as SkoWork

### Requirement: Product Rename Compatibility
The renamed client SHALL continue using existing user data, credentials, settings, single-instance coordination and integration identifiers. Existing launch commands using the former executable name SHALL open SkoWork without creating a second profile or a second active client.

#### Scenario: Upgrade an existing profile
- **WHEN** a user with an existing Workbench profile launches SkoWork
- **THEN** their Threads, knowledge, sign-in and preferences remain available and existing citations still open

#### Scenario: Old launch command
- **WHEN** an existing shortcut launches the former executable name while SkoWork is running
- **THEN** it activates the same client instance and preserves the shortcut's supported launch arguments

### Requirement: Speech Release Availability
Every published desktop version, including a development version, SHALL have a compatible optional speech package with authenticated metadata and offline import support under client/speech-models. A core-only installation SHALL provide the setup path without bundling the large models into the core package.

#### Scenario: Fresh published package
- **WHEN** the user runs a published desktop zip on a machine without a development checkout
- **THEN** speech setup can acquire its compatible package or import the matching offline package, and does not depend on developer model paths

#### Scenario: Missing speech artifact
- **WHEN** a release lacks a verified compatible speech package or its acquisition metadata
- **THEN** release validation fails instead of publishing a desktop version with an unusable setup action
