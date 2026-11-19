## 1. Runtime and scheduler

- [x] 1.1 Return the start-gate code offline for a disconnected client (D1), queue requests in the Thread while offline, and drain on reconnect with the gate checked before delivery (D2). Verify with runtime tests: a request sent offline is queued, starts after reconnect, and a failed start keeps it queued
- [x] 1.2 Keep Occurrences pending without an attempt while offline, at most one per schedule Trigger (D5). Verify with scheduler tests using the test clock: a day offline ends in one Run after reconnect, with no failed attempt
- [x] 1.3 Verify the session survives refresh failures caused by the network (Session Kept While Offline) with a token-manager test over 24 hours of the test clock

## 2. Desktop app

- [x] 2.1 Show "Waits for the connection" on queued messages and the composer hint while offline (D3), "Offline since" in the status bar (D4), and the not-available-offline wording (D6); strings in en, de, cs. Verify with hidden-window UI tests

## 3. Acceptance

- [x] 3.1 On the local stack: stop the control plane, write a request in a new Thread and in an existing one, read Threads, search, open a note and a Local app; start the control plane and check that both requests run. Record the result in design.md
- [x] 3.2 Run `openspec validate --all --strict`
