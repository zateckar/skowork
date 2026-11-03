# Spec Delta

## Purpose

Defines local apps: small web applications the agent builds for the user, kept each in its own folder and run by the client for the user alone.

## ADDED Requirements

### Requirement: App Folder
Each Local app SHALL live in its own folder under the user's apps folder in the user profile. The folder holds a manifest with name, description, entry file and the MCP tools the app uses, the app's server code, its static files and its SQLite database. Deleting an app deletes its folder.

#### Scenario: Two apps
- **WHEN** the agent creates the apps "sprint-board" and "q3-slides"
- **THEN** each has its own folder with its own manifest and database, and neither can read the other's files

### Requirement: Built-In Modules Only
A Local app SHALL use only the built-in modules of the bundled Node.js runtime and its own files. The client MUST refuse to start an app whose manifest declares package dependencies, and the app process MUST NOT read files outside its folder and the runtime, open network connections, start other processes or load native add-ons.

#### Scenario: Dependency declared
- **WHEN** an app folder contains a package manifest with dependencies
- **THEN** the client does not start the app and tells the user that apps cannot use packages

### Requirement: App Runtime
The client SHALL run each Local app with the bundled Node.js runtime as a process contained by client/tool-sandbox, with no network access. The app server accepts requests only from the client. When the optional runtime package is missing or policy.apps.mode is off, apps are not started and the apps.* tools are not offered; policy.apps.mode takes on or off, default on.

#### Scenario: App tries the network
- **WHEN** an app's server code requests a page on the internet
- **THEN** the request fails, and the app keeps running

### Requirement: Owner-Only Access
The client SHALL open a Local app in the user's default browser at a loopback address with a single-use launch token, which becomes a session for that browser. Requests without that session, from other users of the computer or from other machines, MUST be refused.

#### Scenario: Another local user
- **WHEN** another Windows user on the same computer requests the app's address
- **THEN** the request is refused

### Requirement: MCP Bridge
A Local app SHALL be able to call MCP tools through the client, limited to read-risk tools of the user's MCP servers that its manifest names and the user confirmed. The user confirms the list before the first start and again when it changes. Each call is decided by client/policy-enforcement as a call of the user with the app as origin, and results are returned as JSON. With policy.apps.mcp off the bridge refuses every call; policy.apps.mcp takes off or read, default read.

#### Scenario: Live MCP view
- **WHEN** an app that names mcp:jira/search_issues, confirmed by the user, requests it with a query
- **THEN** the client calls the tool as the user and returns the result to the app

#### Scenario: Unlisted tool
- **WHEN** an app requests a tool its manifest does not name, or a tool that is not read-risk
- **THEN** the bridge refuses the call and the app receives an error

### Requirement: App Lifecycle
The client SHALL start a Local app when the user opens it, stop it after 15 minutes without requests, run at most 3 apps at a time, and keep the last 1 MB of each app's output as its log. Resource limits come from client/tool-sandbox.

#### Scenario: Idle stop
- **WHEN** no request reaches a running app for 15 minutes
- **THEN** the client stops its process, and opening the app again starts it

### Requirement: Apps View
The client SHALL list Local apps in an Apps view with name, description, size and last use, and let the user open, stop, show the folder, delete after confirmation, and ask the agent to change an app, which starts a Thread about that app.

#### Scenario: Delete
- **WHEN** the user deletes an app and confirms
- **THEN** the app is stopped and its folder removed

### Requirement: App Tools
The client SHALL offer the agent apps.create (write-local), which creates the folder and manifest, apps.list and apps.logs (read), apps.open and apps.stop (write-local), and apps.delete (destructive). While a Run builds an app, the agent may read and write files in that app's folder with the files.* tools.

#### Scenario: Build a presentation
- **WHEN** the user asks for a presentation about the Q3 results as an app
- **THEN** the agent creates the app with apps.create, writes its files, opens it with apps.open, and the user's browser shows it
