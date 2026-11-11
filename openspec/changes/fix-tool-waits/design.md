# Design

## Context

The diagnosis was checked in the code on 2026-11-10. Line numbers are as read on this date.

**Child Run waits (problem 1).** Confirmed.
- A subagent or a Skill run starts a child Run inside the parent's tool batch (`client/src/runtime/runtime.cpp:1417-1419`). An Action item keeps the id of the Run that raised it (`client/src/runtime/types.h:77`).
- An interrupt of a child Run interrupts its ancestors (`runtime.cpp:914-921`, `subtree_has_open_actions` at `runtime.cpp:2148-2153`).
- The Inbox counts every open Action item of the Thread (`client/src/inbox/inbox_store.cpp:17`, `:46`). So the Thread is listed under Action, but no card is drawn.
- The feed draws Action cards only for standalone Action items (`client/src/shell/feed.cpp:202-209`) and for events of top-level Runs (`feed.cpp:156-158`). `threads.feed` returns child Runs without their events (`client/src/inbox/inbox_service.cpp:513-520`). The subagent rail shows only the child's state (`feed.cpp:315-328`). The user therefore cannot approve the child's Approval.
- No limit exists. No expiry exists for runtime Action items. The only expiry is the 2-minute one for meeting Questions (`client/src/meeting/meeting_service.h:146`). Add-agent-runtime says that Unattended Approvals have no timeout (`openspec/changes/add-agent-runtime/specs/client/agent-runtime/spec.md:176-177`).
- Correction to the diagnosis: none. Cancelling the parent already cancels child Runs and resolves their open Action items (`runtime.cpp:2311-2326`). This is covered by add-agent-runtime Cancellation and is not restated here.

**Local app write (problem 2).** Confirmed, with one correction.
- `AppManager::start_locked` runs with `AppManager::mx_` held (`client/src/apps/apps.cpp:545`, `:549`). It waits for the app's ready signal with `r->cv.wait_for(..., env_.start_timeout)` (`apps.cpp:643-644`). The default timeout is 10 seconds (`client/src/apps/apps.h:92`). The lock is not released during the wait.
- Each browser request calls `connect`, which calls `start_locked` again for an app that is not running (`client/src/apps/app_server.cpp:381`, `apps.cpp:718-720`). A broken app therefore holds the lock on every request.
- `folders_for` takes `mx_` (`apps.cpp:481-482`). The policy enforcer calls it while it builds the rules for a call (`client/src/policy/enforcer.cpp:30`, `:71`), through `client/src/app/app.cpp:1184-1186`. `create` and `update` also take `mx_` (`apps.cpp:445`, `:465`).
- Correction to the diagnosis: the write itself is fast. The wait happens in the policy check and in the folder lookup that the write needs. `apps.create` and `apps.open` are blocked by the same lock.

**Other specs.** `fix-long-thread-model-calls` modifies Run Lifecycle States, Run Context, Lazy Tool Loading and Run Retry in `client/agent-runtime`. This change adds requirements with new names and does not touch those. No change in flight modifies the feed or the app manager.

## Decisions

**D1. Child Run cards are shown in the parent Thread feed.** The feed shows Approval and Question cards of an interrupted child Run under the subagent entry that started it, with the same actions as a top-level card. The `threads.feed` call loads the events of each interrupted child Run that waits, meaning that it, or a subagent it started, has an open Action item.

Placement, decided during implementation: the feed has no entry for a subagent (its `subagent` event is shown in the Activity rail only), so the card sits directly after the tool call that started the child (`delegate` or `use_skill`), which is the subagent's entry in the feed. A child whose starting call is not in the feed gets its card at the end. The Activity rail is unchanged. A live `action.created` of a child makes the feed reload, because the card needs the child's log. Alternatives considered:
- Show child Action items only in the Action view. Rejected: the user still cannot see the tool call in context.
- Make the child a top-level Run. Rejected: it breaks the parent and child model and the subagent tool result.
- Approve child Approvals automatically when the parent is approved. Rejected: it loosens the decision that policy and the Approval made.

**D2. An unanswered child Approval expires after 30 minutes, in an attended Run.** The runtime resolves it as rejected and writes the expiry to the Activity log. The stored resolution is `{kind: reject, expired: true}`; the flag selects the result text, so the four resolutions of Approval Interrupts stay as they are. The child receives the tool result "The user did not answer in time, so the action did not run." and continues, so it can finish other work or report the gap. Alternatives considered:
- Add a fifth resolution, "expired". Rejected: Approval Interrupts defines four resolutions, and a fifth would change that requirement.
- Approve after a timeout. Rejected: it runs a call the user did not approve.
- End the child at once. Rejected: the child may still complete useful work.
- Apply the expiry to top-level Approvals too. Not chosen here; it is an open question.

**D3. A child Run ends after 60 minutes in an attended Run, waiting included.** The limit covers Skill runs and subagents. When it is reached, the runtime cancels the child's open Action items, ends the child as failed with reason "subagent time limit reached", and gives the parent the failure as the result of the call that started the child. This limit is separate from `wall_minutes` in Run Limits, which still excludes interrupted time. Unattended child Runs are not limited, because add-agent-runtime says their waits have no timeout. The values are fixed in this change. Implementation: `AgentRuntime::expire_child_waits` cancels the open Action items of a waiting child and of its subagents, cancels those subagents, and resumes the child; the child then ends as failed at its next step, before its next model call, so a child that is executing at the limit ends after its current model or tool call. Alternative considered: two policy keys under `policy.llm`. Rejected for now, because each policy key needs an owning capability and an update of the key list in `openspec/config.yaml`. This is an open question.

**D4. The app manager does not hold its lock while an app starts.** `start_locked` waits for the ready signal without holding `AppManager::mx_`. A per-app starting state makes a second request for the same app wait for the first start, so only one process is started. `folders_for`, `list`, `get`, `create` and `update` never wait for a start. The start timeout stays at 10 seconds, so a broken app still returns its error within that time. Alternatives considered:
- Shorten the start timeout. Rejected: the lock would still block every request for the timeout, and the cause would remain.
- Use a separate mutex for the grants only. Possible, and left to the implementer. The spec states the behaviour, not the lock layout.
- Start apps on a worker thread and return at once. Rejected: the browser request needs the result of the start.

Implementation: `start_locked` registers a start in progress, releases the manager lock while it spawns the process and waits for the ready signal, and takes the lock again to publish the result. A request for the same app waits for that start and gets its result. `remove` waits for a start in progress, so a deleted app is not left running. Starts in progress count toward `max_running`.

**D5. A write completes within 2 seconds during an app start.** This is the observable bound that the test in task 3.3 checks. It leaves time for the policy check and the write itself on the baseline hardware.

**D6. Expiry is checked by the application every 30 seconds.** The runtime has no timer of its own. The window's one-second tick posts `expire_child_waits` to the background pool every 30 ticks (`client/src/app/app.cpp`). An Approval therefore expires up to 30 seconds after its 30 minutes, and the spec says so.

## Risks / Trade-offs

- An expired approval may surprise the user. The Activity log shows it, and the child can ask again. The 30-minute value is a guess and may need tuning.
- Child cards add a read of the child events in `threads.feed`. The read is limited to interrupted child Runs, so the cost stays small.
- The lock change can expose races in the app manager. The tests in tasks 3.1 to 3.2 cover the new paths. Review the state changes under `AppManager::mx_` with care.
- Attended-only limits leave Unattended waits without a bound, which add-agent-runtime requires.

## Decided Questions (user, 2026-11-10)

1. Top-level Approvals do not expire in this change.
2. The child limits (30 and 60 minutes) stay fixed values, not policy keys. They can become policy keys later if needed.
3. A failed app start has no cooldown in this change. With D4 the start no longer holds the lock, so a broken app no longer blocks file writes.

## Glossary Requests

- "Child Run": a Run started by a parent Run, either a subagent or a Skill run. The term is used in add-agent-runtime but is not in the glossary. Request for `openspec/config.yaml`.
- "Approval" is used in the glossary definition of Action item but has no entry of its own. Request for `openspec/config.yaml`.

## Acceptance

Recorded 2026-11-11 after task 4.1 (`cmake --build --preset test`, then `ctest --test-dir build/test -E "computer_win_tests|e2e_tests"`).

- **Before:** 17/17 passing. Gtest cases: `win_tests` 717, `ui_tests` 66.
- **After:** 17/17 passing. Gtest cases: `win_tests` 724 (+3 child Run waits, +1 inbox feed, +3 app start), `ui_tests` 67 (+1 feed card).
- **Fails before the change:** the change's tests were checked against the change's absence by disabling one piece at a time (the working tree is shared, so the commit could not be reverted). With `expire_child_waits` returning at once, `ChildRunApprovalExpiresAfterThirtyMinutes` and `ChildRunQuestionEndsAtSixtyMinutesWithTheFailureAsTheCallResult` fail on their 10-second waits. With the child card placement off, `ShellFeed.ChildRunApprovalIsACardUnderTheCallThatStartedTheSubagent` fails. With the start holding the manager lock, `AppsManager.FolderAccessAndAppChangesDoNotWaitForAPendingStart` and `AppsTools.WriteToAnAppFolderCompletesWhileAnotherAppStarts` fail. Each change was reverted and the full suite run again.
- **Not run:** task 2.4 and task 3.5 (they drive the desktop client, so they need the user's OK). Task 4.3 (archive) waits for them.
- `openspec validate --all --strict`: 49 of 49 pass, including `fix-tool-waits`.
