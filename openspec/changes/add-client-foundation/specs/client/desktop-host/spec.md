# Spec Delta

## Purpose

Defines how the Skowork client runs on a user's Windows laptop: process lifecycle, tray presence, the on-demand UI window, the local API, and the footprint budgets that keep it lightweight.

## ADDED Requirements

### Requirement: Per-User Resident Process
The client SHALL run as a single resident process per interactive user session. The process runs with the user's own standard (non-elevated) token and starts automatically at user logon without requiring administrator rights.

#### Scenario: Start at logon
- **WHEN** the user signs in to Windows
- **THEN** the client process starts within the user's session using the user's non-elevated token
- **AND** no Windows service or administrator-installed component is required for it to run

#### Scenario: Second user on the same machine
- **WHEN** two users are signed in to the same machine via fast user switching
- **THEN** each user has an independent client process that cannot access the other's process, data or local API

### Requirement: Single Instance
The client SHALL allow at most one running instance per user session. A second launch MUST hand its arguments (for example "open window" or a deep link) to the running instance and then exit.

#### Scenario: User launches the client again
- **WHEN** the client is already running and the user starts it again from the Start menu
- **THEN** the running instance brings its window to the foreground
- **AND** the second process exits without starting another agent runtime

### Requirement: Per-User Registration
The client SHALL register itself for the current user only, without administrator rights, to start at logon. It SHALL repair this registration at every start when it points to a different executable path.

#### Scenario: Moved installation
- **WHEN** the distribution system moves the client to a new folder and the user starts it
- **THEN** the start-at-logon entry points to the new executable

### Requirement: Tray Presence
The client SHALL show a notification-area (tray) icon while running. The icon's menu offers Open, New thread, Pause background work, Status and Quit. The icon state indicates disconnected, idle, running and action-required.

#### Scenario: Action required indication
- **WHEN** at least one Thread has an Action item
- **THEN** the tray icon shows the action-required state
- **AND** its tooltip shows the number of Action items

#### Scenario: Quit from tray
- **WHEN** the user chooses Quit
- **THEN** the client asks for confirmation if any Run is active, then stops all Runs and worker processes and exits

### Requirement: On-Demand UI Window
The client SHALL host its user interface in a window that is created only when the user opens it. The window's rendering resources MUST be released when the user closes it, while the client keeps running in the tray.

#### Scenario: Close window keeps agent running
- **WHEN** the user closes the main window while a Run is active
- **THEN** the Run continues in the background
- **AND** the window's rendering processes are terminated

### Requirement: Local API Restricted To Current User
The client SHALL expose its local API only through an interprocess channel that the current user's processes can reach, and MUST NOT listen on any TCP or UDP port for its local API. The only listening sockets the client may open are the egress proxy defined by client/tool-sandbox and the sign-in loopback redirect of client/identity while a sign-in is pending.

#### Scenario: Other user attempts connection
- **WHEN** a process running as a different user tries to connect to the client's local API
- **THEN** the connection is refused by the operating system

#### Scenario: No listening ports
- **WHEN** the client is running with the UI window closed, no sign-in pending and no sandboxed process running
- **THEN** a listing of listening sockets shows no socket owned by the client process

### Requirement: Footprint Budgets
On the baseline hardware, the client SHALL stay within these budgets:
- the core package installs in 30 MB or less, including its storage, search and document libraries;
- the optional speech-to-text package installs in 40 MB or less of runtime plus at most 1.2 GB of models;
- the core process uses a working set of 60 MB or less when idle with the window closed;
- average CPU is 0.5% or less when idle;
- the tray is ready 2 seconds or less after process start.

#### Scenario: Idle budget check
- **WHEN** the client has been idle for 10 minutes with the window closed and no background job running
- **THEN** the core process working set is at most 60 MB and its average CPU over the period is at most 0.5%

#### Scenario: Models budget covers all local models
- **WHEN** the optional speech-to-text package is measured
- **THEN** the speech-to-text models with their fallback, and the voice-activity and diarization models, together take at most 1.2 GB, and the package contains no other model

#### Scenario: Core works without speech-to-text package
- **WHEN** the optional speech-to-text package is not installed
- **THEN** the client runs, and only meeting transcription reports that it is unavailable

### Requirement: Worker Process Lifetime
The client SHALL run CPU-heavy or risky background work, such as transcription, knowledge indexing and document extraction, in worker processes separate from the core process. A worker MUST exit after it has been idle for 60 seconds. Other capabilities rely on this rule rather than defining their own.

#### Scenario: Worker released after job
- **WHEN** a background job (for example transcription or indexing) completes and no other job is queued for 60 seconds
- **THEN** the worker process that ran it has exited

### Requirement: Worker Memory Budgets
On the baseline hardware, each worker process SHALL stay within its peak working-set budget:
- the speech-to-text worker: 2 GB;
- the indexing worker (chunking and index writes; it runs no ML model): 512 MB;
- the document extraction worker: 1 GB.

A worker that exceeds its budget is terminated, and its job fails with a resource-limit error. Other capabilities rely on these budgets rather than defining their own.

#### Scenario: Extraction worker exceeds budget
- **WHEN** extracting a malformed document drives the extraction worker above 1 GB
- **THEN** the worker is terminated, the extraction fails with a resource-limit error, and the core process keeps running

### Requirement: Pause Background Work
The client SHALL let the user pause all background work for a chosen duration: indexing and embedding, transcription, mail and calendar sync, and Trigger firings. client/scheduler defines how Trigger firings suppressed by a pause are caught up afterwards.

#### Scenario: User pauses for an hour
- **WHEN** the user chooses "Pause background work" for 1 hour
- **THEN** no indexing, transcription, mail or calendar sync, or Trigger Occurrence starts for one hour, and the tray shows the paused state

### Requirement: Background Work Deferral
The client SHALL defer background indexing and embedding, background transcription, and mail and calendar sync while battery saver is on, and during full-screen or presentation mode. Trigger firings are not deferred. Battery-saver deferral applies unless policy.ui.background_on_battery is true (default false). Work the user starts explicitly is never deferred. Other capabilities rely on this rule rather than defining their own.

#### Scenario: Battery saver
- **WHEN** Windows battery saver turns on while an indexing job is running
- **THEN** the job is suspended and resumes automatically when battery saver turns off

#### Scenario: Presentation
- **WHEN** the user starts a full-screen presentation
- **THEN** background indexing and mail sync are suspended until the presentation ends

#### Scenario: Trigger on battery saver
- **WHEN** a scheduled Trigger fires while battery saver is on
- **THEN** its Occurrence starts normally, because Trigger firings are not deferred

#### Scenario: User-started job
- **WHEN** the user explicitly starts transcription of a recording while battery saver is on
- **THEN** the job runs without being deferred

### Requirement: Version Reporting
The client SHALL report its product version, build channel and commit identifier in the UI's About view, on the local API, and to the control plane during registration.

#### Scenario: About view
- **WHEN** the user opens About
- **THEN** the version, channel and commit identifier are shown and can be copied

### Requirement: Third-Party Notices
The client SHALL list, in the UI's About view, every third-party component and model it ships. Each entry gives the name, version, licence, and any attribution the licence requires. Components of the optional speech-to-text package are listed as part of that package.

#### Scenario: Speech model attribution
- **WHEN** the user opens About
- **THEN** the Parakeet speech model is listed with its CC-BY-4.0 licence and its attribution to NVIDIA, as part of the speech-to-text package

### Requirement: Diagnostic Logs
The client SHALL write rotating diagnostic logs to its data directory, capped at 50 MB in total. Logs MUST NOT contain message content, file content, tokens or credentials unless the user enables verbose diagnostics for a limited time.

#### Scenario: Log rotation
- **WHEN** the logs reach the 50 MB cap
- **THEN** the oldest log files are deleted first

#### Scenario: No secrets in logs
- **WHEN** the client authenticates to the control plane
- **THEN** no access token or ticket value appears in any log file
