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

### Requirement: Speech Package Catalog
The client SHALL take the speech package catalog only from the Policy bundle: policy.speech.package.url (https address of the archive), policy.speech.package.sha256, policy.speech.package.version and policy.speech.package.hosts (further hosts a download may be redirected to). All default to empty. While the address or the digest is empty, Download is unavailable.

#### Scenario: Administrator publishes a package
- **WHEN** the effective policy names an address and a digest and the user chooses Download
- **THEN** the client downloads the archive from that address

#### Scenario: No catalog
- **WHEN** the effective policy has no package address
- **THEN** Download is unavailable with an explanation that the administrator has not published a speech package

### Requirement: Speech Package Digest
The client MUST refuse a downloaded or imported archive unless its SHA-256 equals policy.speech.package.sha256.

#### Scenario: Downloaded archive differs from the catalog
- **WHEN** a download completes and its SHA-256 differs from policy.speech.package.sha256
- **THEN** the package is not activated and the previous package stays active

#### Scenario: Imported archive differs from the catalog
- **WHEN** the user imports an archive whose SHA-256 differs from policy.speech.package.sha256
- **THEN** the import is refused and the previous package stays active

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
Speech acquisition MUST use only the authenticated package catalog of Speech Package Catalog or an offline package verified against it. It MUST NOT accept arbitrary package URLs from model output, extracted content or a Tool call, or send identity tokens to package hosts outside the configured control plane.

#### Scenario: Redirect to a different host
- **WHEN** a package download redirects to a host outside the authenticated catalog's permitted origins
- **THEN** acquisition stops without forwarding credentials or activating any downloaded content

### Requirement: Speech Package Location
The client SHALL load the speech runtime and models only from the activated speech package under the user-private data root (`<data root>\speech\packages\<version>\models\stt`). It MUST NOT resolve speech files beside the executable, from environment variables, or from the source tree, in any build. Development test engines remain compiled only into development builds and load no model files.

#### Scenario: Client without administrator rights
- **WHEN** a verified package is activated under the data root and the client runs from a folder the user cannot write
- **THEN** the worker loads the runtime and models from the activated package and reads no speech file from the executable's folder

#### Scenario: No package activated
- **WHEN** no complete, compatible package exists under the data root
- **THEN** the worker reports model_missing without searching any other location

### Requirement: Speech Versions And Selection
Each activated package SHALL occupy its own versioned folder. The client SHALL use the newest complete package whose manifest is compatible with it (schema 1, package skowork-speech, architecture windows-x64). A package that has not finished activation SHALL NOT be selected.

#### Scenario: Failed activation keeps the previous package
- **WHEN** a newer package fails verification or engine initialization during setup
- **THEN** its folder is removed and the previously activated package remains the one in use

#### Scenario: Removal while in use
- **WHEN** the user removes or repairs a package while a capture or transcription uses it
- **THEN** its files stay in place, the folder is marked for removal, and it is deleted at the next start

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
