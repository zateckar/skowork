# Proposal

## Why

On 2026-11-01 the product owner asked that the desktop app keep working, in a limited way, for at least 24 hours without the control plane: on a train, off VPN, or while the control plane is down. User data is already local (client/local-storage), and the Policy bundle stays valid for 7 days (control-plane/policy-service Bundle Versioning And Expiry). Two things still break during a longer outage:

- The composer refuses new requests while the client is disconnected (client/control-channel Offline Operation), so the user cannot even write down the work they want done.
- A Trigger that fires while the client is disconnected counts as a failed attempt. After three firings it becomes dead, so a day off the network kills scheduled work.

The model is reached only through the control plane's gateway, and the credentials of the organization LLM API never reach laptops (control-plane/llm-connections). So Runs cannot execute offline. The product owner chose to queue requests until the connection returns.

## What Changes

- While disconnected, everything that works on local data keeps working with no time limit of its own: Threads, search, the Knowledge view and knowledge.search over local Sources, notes, settings, Local apps, and reading Chats.
- A request sent while disconnected waits in its Thread as a queued message marked "Waits for the connection". The user can edit or remove it. It starts a Run when the client is connected again, in the order written. A new Thread can be started the same way.
- The status bar says since when the client is offline. Features that need the control plane (sharing, team knowledge, sign-in, policy updates, the directory of colleagues) say they are not available offline.
- A failed token refresh caused by the network never ends the session. Only a rejection by the identity provider does, as today.
- An Occurrence that cannot start because the client is disconnected stays pending without counting an attempt, and starts when the connection returns. Further firings of the same schedule Trigger while it waits are recorded as skipped.

Out of scope:
- Running the agent offline (no local model, no LLM credentials on laptops).
- Changing the Policy bundle validity, a control-plane server setting.
- Mail and calendar, which talk to Microsoft Graph and not to the control plane (client/mail-calendar Offline Mail Behaviour).

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/control-channel`: Offline Operation (modified).
- `client/identity`: Session Kept While Offline (added).
- `client/scheduler`: Occurrences While Disconnected (added).

## Impact

- **Roadmap:** 20, after add-local-apps. Depends on add-client-foundation, add-agent-runtime, add-work-inbox and add-scheduled-and-remote-tasks.
- **Code:**
  - client/src/runtime (queueing a request while the start gate reports the client offline, draining on reconnect);
  - client/src/app (the offline reason and since-time, reconnect hook);
  - client/src/scheduler (pending without an attempt);
  - client/src/shell (queued message label, status bar, composer hint);
  - strings in en, de, cs.
- **Data:** none new; queued messages use the existing queue of the Thread.
