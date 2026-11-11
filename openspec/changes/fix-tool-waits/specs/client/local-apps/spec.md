# Spec Delta

## ADDED Requirements

### Requirement: Folder Access During App Start
The client SHALL keep folder access, app creation and app changes available while a Local app is starting, and MUST NOT make them wait for that start. A file write to a Local app folder that the user approved SHALL complete within 2 seconds, even while another app is starting or fails to start. A browser request for an app that is starting waits for that start, up to the start timeout, and a failed start returns its error within that timeout.

#### Scenario: Write while a broken app starts
- **WHEN** the user approves a write to the folder of a new app whose server does not start, and the browser requests that app at the same time
- **THEN** the write completes within 2 seconds, and the browser request gets the start error within the start timeout

#### Scenario: Two requests for one app
- **WHEN** two browser requests for the same stopped app arrive while it starts
- **THEN** the client starts one process, and both requests wait for that single start
