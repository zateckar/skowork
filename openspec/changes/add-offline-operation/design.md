# Design

## Context

- The runtime refuses to start a Run through its start gate. The gate reports one reason: not signed in, not registered, locked, version below the minimum, or disconnected. send_message starts a Run when the Thread has none, and queues the message only while a Run is active.
- The Thread queue (queued messages with edit and remove) already exists. When a Run ends, the oldest queued message starts the next Run.
- The scheduler treats every start error other than thread_busy, not_started and stopping as a failed attempt.
- The token manager keeps the session on network errors and ends it only on invalid_grant (client/identity Refresh Token Protection And Rotation). So the session already survives an outage; this change makes it a stated requirement.

## Decisions

### D1. Offline is a start-gate reason with its own code
- **Choice:** the start gate returns the code `offline` when the client is disconnected or cannot renew its access token because the identity provider is unreachable. Locked, version-blocked and not-registered keep the code `blocked`. The runtime handles `offline` in send_message by queueing the message in the Thread, even when the Thread has no active Run, and emits queue.changed.
- **Alternatives considered:**
  - A separate outbox table for offline requests (a second queue next to the Thread's queue, with its own edit and remove).
  - Starting the Run and letting it fail on the model call (a failed Run per request, and retries burn the Run's limits).

### D2. Draining on reconnect
- **Choice:** when the control channel reports connected, the app calls the runtime's `resume_waiting()`. For every Thread with queued messages and no active Run, it starts a Run with the oldest message, in the order of the messages' creation. The gate is checked before a message is marked delivered, so a message is never lost when the start fails. The same check is added to the existing after-Run path.
- **Alternatives considered:** draining lazily when the user opens the Thread (offline requests would wait for the user without reason).

### D3. Queued-while-offline label
- **Choice:** the feed shows queued messages as today. While the client is offline, their label reads "Waits for the connection" instead of "Queued". A message is not tied to how it was queued: whatever is queued when the client connects is started. The composer hint under the box says "You're offline. Your request will start when Skowork is connected again."
- **Alternatives considered:** a per-message flag "queued offline" (it adds state; the label can be derived from the connection state).

### D4. Status bar since-time
- **Choice:** the control channel records when it entered the disconnected state; the status JSON carries `control_plane.offline_since` (Unix ms, 0 when connected). The status bar label reads "Offline since 14:05", or with the date when it began on an earlier day.
- **Alternatives considered:** a banner (the status bar already reports the connection; a banner would push the content down for a normal situation).

### D5. Occurrences wait without an attempt
- **Choice:** the scheduler treats the start error `offline` like `thread_busy`: the Occurrence stays pending with a retry time 60 seconds later, no attempt is counted, and the Run starts once the gate is open. A schedule Trigger keeps at most one pending Occurrence. A firing while one is pending is recorded as skipped (missed), which matches the catch-up rule.
- **Alternatives considered:** counting offline as a failure with a longer backoff (a long outage still ends in dead Occurrences).

### D6. Features that need the control plane
- **Choice:** the views that call the control plane show their existing error state, worded as "Not available while Skowork is offline": sharing, team knowledge note, the colleague directory, sign-in and policy refresh. No feature is hidden.
- **Alternatives considered:** disabling the controls (users would not learn why).

## Risks / Trade-offs

- [Many queued requests start at once on reconnect and hit the gateway's concurrency limits] → Runs go through the existing slot and queue rules of the runtime; at most the usual number run at a time.
- [A request queued yesterday starts in a changed context] → The message stays visible and editable while it waits, and its Thread shows when it started.
- [The user expects an answer offline] → The composer hint and the queued label say plainly that the request waits for the connection.

## Acceptance (3.1, 2026-11-01)

On the local stack with the demo client (real organization LLM API through the control plane's gateway):
- Stopped the control plane. After the 60 s grace the client reported `agent.offline: true`, the status bar read "Offline since 7:40 PM", and the session stayed signed in.
- Sent a request in a new Thread and one in an existing Thread. Both were queued (no Run); the existing Thread's composer showed "Waits for the connection" and the offline hint. Reading a Thread's feed, search, the Knowledge notes list and the Local apps list worked offline.
- Started the control plane. The client reconnected in 12 s, logged "2 waiting request(s) started after reconnecting", and both Runs succeeded with the expected answers.
- Found and fixed: a long queued message did not wrap, so its Edit and Remove buttons ran past a narrow reading pane. The text now wraps (checked in the offline composer test, which fails without the fix).
- Push and identity behaviour while offline are covered by unit tests (`TokenManagerOffline.*`: a day of failed refreshes keeps the session; the first refresh afterwards renews it, a refused one ends it). The scheduler keeps one pending Occurrence per schedule Trigger without counting an attempt (`Scheduler.OccurrenceWaitsOfflineWithoutAnAttempt`).
