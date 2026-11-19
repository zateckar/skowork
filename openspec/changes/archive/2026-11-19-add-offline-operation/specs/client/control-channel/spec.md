# Spec Delta

## MODIFIED Requirements

### Requirement: Offline Operation
While disconnected, the client SHALL keep every feature that works on local data available, with no time limit of its own and for at least 24 hours: reading and searching Threads, the Knowledge view and knowledge.search over local Sources, notes, settings, Local apps, and reading Chats (client/user-messaging Offline Sending covers messages written offline). It MUST NOT start Runs while disconnected. A request the user sends while disconnected, in an existing Thread or as a new Thread, SHALL wait in that Thread as a queued message labelled as waiting for the connection, which the user can edit or remove. Queued messages start their Runs when the client is connected again, in the order they were written. The status bar SHALL say since when the client is offline. Features that need the control plane SHALL say that they are not available while offline.

#### Scenario: Off VPN
- **WHEN** the client is in the disconnected state
- **THEN** no Run starts, and local search and Thread reading still work

#### Scenario: Request written offline
- **WHEN** the user sends "Summarise the brake notes" while disconnected, and the client connects again an hour later
- **THEN** the message waits in its Thread labelled as waiting for the connection, and its Run starts after the client connects

#### Scenario: A day offline
- **WHEN** the client has been disconnected for 24 hours with a valid cached Policy bundle
- **THEN** Threads, the Knowledge view, notes, Chats and Local apps still open, and the status bar shows when the client went offline
