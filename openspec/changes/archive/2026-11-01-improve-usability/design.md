# Design

## Context

Findings come from running the stack on 2026-10-31 and from an audit of every user-facing requirement against the two UIs. Most gaps are missing UI for behaviour that already exists behind an RPC or API.

## Decisions

### D1. Withheld tools are explained in the search result, not offered
- **Choice:** find_tools also searches the tools that are not offered because of policy. Matches are not loaded; the result gets one line per withheld capability: "Computer use (computer.*) is switched off by policy.tools.computer.mode. An administrator can enable it." Capabilities are grouped by tool-id prefix, and the key comes from the enforcer (the mode key for shell and computer use, otherwise the matched rule or Risk class default).
- **Why:** the model then tells the user what is missing and who can change it. It still cannot call the tool, so the policy decision is unchanged.
- **Alternatives considered:** listing withheld tools in the system prompt (costs tokens on every turn and leaks the full tool list); a client-side banner (the user sees it only after the agent has already given up).

### D2. Structured editor generated from the policy schema
- **Choice:** the console reads the key registry the policy service already serves (`GET /admin/policy/schema?keys=true`: path, owner, type, default, guardrail mode) and renders one control per key: boolean and enum → select with an "inherited" entry, number → number field, list of strings → one entry per line, list of enum values → checkboxes, decision maps and open maps → a pattern/value table, maps with fixed keys → one field per key. Sections follow the top-level key and open when they hold a set key or an error. The guardrail scope shows only keys with a guardrail mode. Both views share one draft; switching from JSON with a syntax error keeps the JSON view. The registry has no prose per key, so each control shows the key, its owning capability, default and range; the owning spec stays the place that explains it.
- **Alternatives considered:** a hand-written form per key (drifts from the schema as keys are added); keep JSON only with help text (rejected by the product owner).

### D3. Layout
- **Choice:** cards in an auto-fill grid with a 420 px column minimum, `min-width: 0` and `overflow-x: auto` on cards, wrapping table cells, and a page max width of 1600 px. Labels use the `<label>` element around each input; a shared `confirm()`-based helper guards destructive actions.
- **Alternatives considered:** a UI component library (new dependency, against "keep it simple").

### D4. Desktop Policy page and Files section
- **Choice:** the Policy page renders `policy.summary` (extended with sources, guardrails, unattended rules and the enforcement level). The Files section uses the shell's folder picker (IFileOpenDialog with FOS_PICKFOLDERS) and the existing `policy.grants.*` RPCs.
- **Alternatives considered:** a link to the admin console (users have no admin access).

### D5. Reaching specified behaviour in the desktop app (task 2.4)
- **Choice:** each gap gets a control where the user already looks, and uses the RPC that exists:
  - Folders (client/work-inbox): a folder view's list header has Folder options with Rename (`folders.rename`) and Delete. Delete asks first, says that the Threads move to the Inbox (`folders.delete`), and then shows the Inbox.
  - Queued messages (client/agent-runtime): each queued message has Edit next to Remove. Edit opens a dialog with the text and calls `queue.edit`.
  - Usage (client/model-access Usage Accounting): when a Run ends, the Thread feed shows a line after that Run with its prompt, completion and reasoning tokens and its number of model calls. The thread snapshot carries `model_calls` and `usage` per Run, and the live `usage` event updates them.
  - Author Withdrawal (control-plane/skill-registry): the skill editor's list of published versions has Withdraw on each version that is published or in review. Withdraw asks first. A new host RPC, `skills.withdraw {name, version}`, calls `POST /api/v1/skills/{name}/versions/{version}/withdraw` on the registry.
  - Item exclusions (client/knowledge-graph Exclusions):
    - Exclude a file… picks files with the system Open dialog and adds one `item` exclusion per file. This works before the file is indexed, and removes it from the store if it was indexed.
    - Forget still excludes an item that is already indexed.
    - The list shows an item exclusion as "Excluded item" with the file path, or with the Source and its id.
- **Alternatives considered:**
  - A context menu on folders in the navigation pane (the navigation has no context menus yet, and keyboard users reach the list header the same way).
  - Usage in the Activity rail (the rail lists subagent Runs only, so a top-level Run would have no place there).

## Risks / Trade-offs

- [The explanation tells the model that a capability exists, which a prompt injection could use to pester the user] → The text is fixed by the client, names only the capability and key, and changes nothing about what the agent may call.
- [Generated controls look generic and carry no explanation] → Each control names its owning capability; descriptions per key can be added to the registry later without changing the editor.

## Acceptance notes (2026-11-01)

Run on the local stack: the dev control plane, the console from the Vite server on :4200 signed in through the organization Keycloak as a platform-admin, and a development client signed in as the same user.

- Admin console:
  - The structured policy editor and the JSON tab agree. Group listing and the revoke confirmation work; cancelling the confirmation leaves all 59 installations.
  - A narrow window keeps every card usable.
- Desktop client:
  - The agent asked to click in an application explained that computer use is off by policy (policy.tools.computer.mode), and the Policy page showed Computer use: Off with its source "default".
  - Files: once policy.filesystem.grantable allowed the folder, a read-only grant let a Run read a file in it. Its write was refused ("not in a folder the agent may write to") and the file stayed unchanged. Revoking removed the grant.
  - Folders: create, rename and delete. Deleting returns the Thread to the Inbox. Queued message: edited while a Run was working; the edited text ran next.
  - Usage: the line after a Run gives prompt, completion and reasoning tokens and the number of model calls.
  - Skill Withdraw: a published version became withdrawn with its end time.
  - Item exclusion: an indexed file left the store when excluded, and came back when the exclusion was removed.
  - These were driven through the RPCs the controls call and checked in window captures. The control rendering is covered by the shell tests.
- Found and fixed:
  - Console table cells broke words in the middle (`overflow-wrap: anywhere`). They now wrap at word boundaries, and buttons in cells do not wrap.
  - Malformed hosts were accepted. The network host lists (policy.network.egress_allow and egress_deny) accepted entries such as `https://bad host/*`, which no client can match. The keys now carry a host pattern, so the console shows "invalid format" next to the field. The admin console e2e test checks it.
  - Client status bar:
    - The policy label showed "Policy" without its number, because the version is a number in the status.
    - The Connections and content telemetry buttons sat lower and larger than the caption labels; they are now text-sized (Button::make_inline).
  - A folder created or renamed outside the window (another window, the pipe) did not appear in the navigation, because only folder deletion announced the change. Every folder change now emits inbox.changed.
- Noticed, not changed:
  - The client Policy page writes some modes in lower case ("sandboxed") next to translated labels ("Off").
  - The Chats directory search has no placeholder.
  - The org guardrail with no version in force shows "Not yet updated: 59 of 59".
