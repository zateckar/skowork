# Spec Delta

## Purpose

Makes the local speech engine and its language models available, verifiable and repairable for Dictation and meeting capture without requiring developer tools or administrator rights.

## ADDED Requirements

### Requirement: Speech Setup Entry
The client SHALL offer Set up speech recognition from Settings and from a composer whose speech package is unavailable, showing supported languages, download size, required disk space and package status before acquisition.

#### Scenario: Clean installation
- **WHEN** a user opens speech setup on a client without models
- **THEN** the view explains that recognition runs on this device and offers Download and Import package with their sizes and requirements

### Requirement: Speech Download Policy
The client SHALL allow user-initiated speech package downloads only while policy.speech.download is on (values on|off; default on). Off blocks downloads without disabling a verified installed package or an offline package import.

#### Scenario: Managed offline deployment
- **WHEN** policy.speech.download is off and speech is not installed
- **THEN** Download is unavailable with a policy explanation, and Import package remains available

### Requirement: Compatible Speech Acquisition
Download and Import package SHALL acquire the complete speech runtime and models compatible with the installed client, with progress, cancellation, retry and actionable network, disk-space and compatibility errors, without administrator rights.

#### Scenario: Interrupted acquisition
- **WHEN** a download is interrupted or the user cancels it
- **THEN** no partial package becomes active, the state distinguishes cancellation from failure, and the user can retry

#### Scenario: Incompatible offline package
- **WHEN** the user imports a package incompatible with the client
- **THEN** setup rejects it before activation and identifies the required compatibility version

### Requirement: Speech Package Integrity
The client MUST verify package authenticity, every required file's integrity and compatibility, and successful local engine initialization before reporting speech ready. Failed verification leaves any previous working package active.

#### Scenario: Corrupted model
- **WHEN** a downloaded model differs from the authenticated package manifest
- **THEN** the package is not activated, no code from it is loaded, and setup offers retry or import

#### Scenario: Files exist but the engine fails
- **WHEN** all required files exist but the engine cannot initialize
- **THEN** setup reports a repairable engine error instead of Ready

### Requirement: Speech Package Trust Boundary
Speech acquisition MUST use only the product's authenticated package catalog or a verified offline package. It MUST NOT accept arbitrary package URLs from model output, extracted content or a Tool call, or send identity tokens to package hosts outside the configured control plane.

#### Scenario: Redirect to a different host
- **WHEN** a package download redirects to a host outside the authenticated catalog's permitted origins
- **THEN** acquisition stops without forwarding credentials or activating any downloaded content

### Requirement: Speech Repair And Removal
Settings SHALL show the active speech version, languages and disk use, with Verify, Repair and Remove actions. Replacement or removal waits for active capture or transcription to finish or be explicitly cancelled, and follows the footprint budgets of client/desktop-host.

#### Scenario: Repair during transcription
- **WHEN** the user chooses Repair while transcription uses the active package
- **THEN** the client explains the active work and does not remove or replace its files until that work ends or the user cancels it

### Requirement: Speech Readiness Refresh
Successful setup SHALL refresh Dictation and meeting-capture readiness immediately without restarting the client, while keeping microphone access checks and recording consent separate from package verification.

#### Scenario: Return to a composer
- **WHEN** setup completes and the user returns to the originating composer
- **THEN** its draft and caret are preserved and Dictate is available if its microphone, policy and capture-state checks pass

### Requirement: Explicit Speech Check
Speech setup SHALL offer a local microphone and transcription check that starts recording only after the user chooses Test microphone, shows recording and stop controls, displays recognized text without sending it, and discards the test audio afterward.

#### Scenario: Verify without recording
- **WHEN** the user verifies or installs a package without choosing Test microphone
- **THEN** no microphone recording starts

#### Scenario: End-to-end speech check
- **WHEN** the user starts the check, speaks in a supported language and chooses Stop
- **THEN** recognized text is shown locally, the test audio is deleted, and no message is sent
