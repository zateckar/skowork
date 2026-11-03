# client/desktop-host (delta)

## MODIFIED Requirements

### Requirement: Local API Restricted To Current User
The client SHALL expose its local API only through an interprocess channel that the current user's processes can reach, and MUST NOT listen on any TCP or UDP port for its local API. The only listening sockets the client may open are the egress proxy defined by client/tool-sandbox, and the sign-in loopback redirects of client/identity and client/mail-calendar while a sign-in is pending.

#### Scenario: Other user attempts connection
- **WHEN** a process running as a different user tries to connect to the client's local API
- **THEN** the connection is refused by the operating system

#### Scenario: No listening ports
- **WHEN** the client is running with the UI window closed, no sign-in pending and no sandboxed process running
- **THEN** a listing of listening sockets shows no socket owned by the client process
