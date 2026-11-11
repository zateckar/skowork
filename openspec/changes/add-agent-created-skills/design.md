# Design

## Context

A user asked the agent to create a skill. The agent asked where to save the file. Code reading on 2026-11-10 confirmed the cause and corrected part of the first diagnosis.

- **No save tool.** The runtime reserves five tool identities: `find_tools`, `ask_user`, `read_attachment`, `delegate` and `use_skill` (client/src/runtime/runtime.cpp:17-24). None saves a Skill.
- **Data root denied.** The client's data root is denied to every tool (client/src/policy/core.cpp:332-333). Granting it is refused (client/src/app/app.cpp:612-615). The first diagnosis was right on this point.
- **Correction: the agent has a writable folder, and it is the wrong one.** The agent workspace is `<data root>\workspace` (client/src/app/app.cpp:1178-1180). The workspace rule is checked before the data root rule (core.cpp:329), so the agent can write there. The file tools resolve relative paths against it (client/src/tools/files.cpp:485). The Skill catalogue reads only `<data>\skills\private` (client/src/skills/catalog.cpp:96). A SKILL.md written to the workspace is therefore never loaded. This is the source of the confusion, not a denial.
- **Correction: saving is not only in Settings.** "Save as skill" is also in the Thread header menu (client/src/shell/shell.cpp:1011), which opens the editor at `#/skills/new?thread=`. Publishing is in the same code (client/src/app/skill_authoring.cpp:152). Both come from add-skills, which is not archived and so not yet in `openspec/specs/`.
- **Confirmed: the agent is not told a location.** The system prompt lists Skill names and descriptions only (client/src/runtime/runtime.cpp:292-294 for the list). It names no folder for Skills.

What this change builds on:
- Tools and their risk classes: client/core-tools and client/policy-enforcement.
- Skill format, validation, the private tier and sharing: client/skills, add-skills.
- Lazy tool loading and the stable prompt prefix: client/agent-runtime, fix-long-thread-model-calls.

## Goals / Non-Goals

**Goals:**
- A user asks for a Skill in chat and gets a saved, usable Skill without giving a path.
- The user approves the text before it is saved.
- Sharing stays with the user, through the Publish dialog that already exists.

**Non-Goals:**
- A new Skill format, a new validator or a new sharing path.
- Giving the agent direct access to the data root.

## Decisions

### D1. One core tool, `skills.save`, that writes through the Skill code
- **Choice:** the agent saves a Skill with one tool, `skills.save`. The tool is an ordinary registered tool, found by `find_tools`. The client writes the files itself through the Skill editor's save path, so tool file policy does not apply to the write.
- **Why:** the agent needs no path knowledge, the validation is the editor's, and the write cannot reach the data root or the workspace.
- **Alternatives considered:**
  - A reserved runtime tool, like `use_skill`: rejected. Reserved tools are excluded from declarable tools and are not a place for Skill writes (runtime.cpp:17-24).
  - `files.write` into the workspace, plus an import step: rejected. It leaves two copies and a path the agent must know, and the import step would need a second tool.
  - `shell.run` with PowerShell: rejected. It bypasses the validation, and it is a `destructive` candidate for some commands.
  - Giving the agent the private folder path: rejected. The data root is denied to tools by design (core.cpp:332-333).

### D2. No path, and the agent is told so
- **Choice:** `skills.save` takes no path. The tool description and one fixed system prompt sentence for top-level Runs that are not Unattended runs say that a Skill needs no file path, that the tool stores it, and that the agent must not ask where to save. `find_tools` returns the tool for a request to create a Skill.
- **Why:** the fixed sentence keeps the stable prompt prefix. It is the same in every Run.
- **Alternatives considered:**
  - A per-Run sentence that names the private folder: rejected. It changes the prompt between Runs and exposes a folder the agent may not use.
  - Only the tool description: rejected. With lazy loading, the model may not search for the tool before it asks the user.

### D3. Saved Skills are usable from the next Run
- **Choice:** a Skill saved in a Run appears in the agent's Skill list from the next Run started, in any Thread. A Run already executing keeps the list it started with. An explicit `/name` typed after the save works at once, because the catalogue reloads on each save (add-skills D10).
- **Why:** the Skill list is fixed when a Run starts, so the stable prompt prefix holds (agent-runtime Run Context Stability). A tool result cannot change the prompt of a Run in progress.
- **Alternatives considered:**
  - Adding the Skill to the running Run's prompt: rejected. It breaks the stable prefix and the Run Context rule.
  - Making the Skill usable at once for `use_skill` too: rejected, because `use_skill` is offered only from the list fixed at Run start.

### D4. Approval through the tool-risk policy, with the full text shown
- **Choice:** `skills.save` declares risk write-local. policy.tools.risk decides it: approve by default. An update is decided the same way. The approval shows the Skill's name, description, instructions in full, declared tools, risk and network, each file's path and size, personal-data findings (D8) and, for an update, the changed lines against the stored text.
- **Why:** a Skill is model-written text that later runs as instructions, so the user reads it before it is stored. Presentation is owned by client/work-inbox, and this change only states what the approval carries.
- **Alternatives considered:**
  - `allow` by default: rejected. Saved text can carry instructions from untrusted content (see the Risks).
  - `external` risk: rejected. Writing a private Skill is local and reversible in the editor.

### D5. Updates only to Skills the agent created
- **Choice:** a save under an existing private name is refused unless the call sets `update` to true, the name is one the agent created (D6), and the new `version` is higher than the stored one. The stored text is replaced, and the result says that the Skill was updated. The tool never changes a Skill the user wrote in the editor.
- **Why:** a user's hand-written Skill must not be overwritten by an agent.
- **Alternatives considered:**
  - Updating any private Skill: rejected, for the reason above.
  - Versioned copies kept in the folder: rejected. Extra files would have to pass the package rules, and the user gets clutter.
  - No updates at all: rejected. Users refine a Skill in chat.

### D6. Provenance in the local store, not in the Skill folder
- **Choice:** the client records each private Skill the agent created in one table of the local store (name, creation time, last update time, Thread id). The table holds no Skill text. Deleting the Skill in the editor removes its record. Implementation: table `agent_skills` in app.db, migration 1600 (client/src/storage/agent_skills.cpp).
- **Why:** a marker file inside the Skill folder would be a top-level entry that the package rules reject when the Skill is later published (add-skills D6, "unknown top-level entry").
- **Alternatives considered:**
  - A marker file in the folder: rejected, as above.
  - A marker in the Skill's frontmatter: rejected. It would show to the user as a field and would be published with the Skill.

### D7. Validation and limits are the editor's
- **Choice:** the tool validates the frontmatter, the name, the description, the version and the 256 KB limit with the Skill editor's validation (client/src/app/skill_authoring.cpp:54). It writes through the editor's save path (`save_private`, client/src/skills/authoring.h:30). File limits are the package limits of add-skills.
- **Why:** one validator for every route into the private tier avoids drift.
- **Alternatives considered:**
  - A second validator for the agent: rejected, as drift is the risk this avoids.

### D8. Secrets are refused; personal data is shown, not replaced
- **Choice:**
  - Text that holds a secret is refused, and the result names the field and not the value. The registry's Automated Skill Scan (control-plane/skill-registry) is not implemented yet, so the client applies these rules, which the registry scan can reuse: a keyed value (api_key, apikey, api-key, secret, password, passwd, access_token, client_secret, private_key) followed by `:` or `=` and a value of 16 or more characters, or 8 or more with a digit and a letter; a known key shape (sk-, ghp_, github_pat_, xoxb-, xoxp-, xoxa-, xoxs-, xoxr-, AIza, AKIA followed by 16 upper-case letters or digits, a PEM private key header, a JSON Web Token); or an unbroken run of 40 or more key characters with a digit and a letter. Found while implementing task 2.1: the scan rules did not exist yet, so the delta spec refers to this list.
  - Personal data (email addresses, Windows and UNC paths, person names from the Thread) is found by the patterns of the Save as skill draft (client/skills Save Thread As Skill). The approval lists the findings. The tool does not replace them.
- **Why:** a secret must never reach a private Skill that may be shared later. Replacing text silently would change what the user approves.
- **Alternatives considered:**
  - Automatic parameterisation, as the Save as skill draft does: rejected here. The draft step has a Keep list the user reviews. The tool has the approval instead, and an automatic change would be shown only after approval.

### D9. Resource files are text only
- **Choice:** `files` is a list of `{path, content}`. The path must be a safe relative path under `scripts/`, `references/` or `assets/`. Binary content is refused. Limits are those of add-skills.
- **Why:** text can be shown in the approval in full. Binary files cannot be checked by a person.
- **Alternatives considered:**
  - Binary files through an attachment: rejected for this change. It needs a second path for the approval.

### D10. Only top-level Runs that are not Unattended runs may save
- **Choice:** `skills.save` is offered only in top-level Runs that are not Unattended runs. Unattended runs and Child Runs get an error and change nothing.
- **Why:** Unattended runs approve write-local calls by default (`policy.unattended.allowed_risk` defaults to read and write-local, client/src/policy/core.cpp:139). A Trigger or Remote task must not create Skills without a person.
- **Alternatives considered:**
  - Allowing unattended saves under an approval policy: rejected. The approval would fall to nobody.

### D11. Sharing is the user's, through the Publish dialog
- **Choice (implementation):** the tool has no share action. After a save, the agent may offer to share. The saved Skill is named in the Thread with a link to its Publish dialog (client/skills Skill Publishing). The user chooses the scope and audience and confirms there.
- **Why:** the Publish dialog already applies `policy.skills.publish`, the audience rules and org review. A second sharing path would copy those rules.
- **Alternatives considered:**
  - A `skills.share` tool with approval: rejected for this change. It would add an external-risk path with its own audience rules, and the user would confirm twice.
  - An inbox Question with Share and Not now: possible later. It is a presentation choice for client/work-inbox.

### D12. One previous version is kept for restore (user, 2026-11-10)
- **Choice:** an update by `skills.save` keeps the replaced version of the Skill (SKILL.md and resource files) beside it in the local store, one version deep. The Skill editor offers Restore previous version, which swaps the two. Deleting the Skill deletes the kept version.
- **Where (implementation):** the kept version is the folder `<data>/skills/previous/<name>/` (SKILL.md and its scripts/, references/, assets/). The catalogue reads only `private/` and the other tiers, so the kept folder is never loaded as a Skill. Restore renames the folders in turn through a `<name>.swap` folder, so a failure puts the current version back.
- **Why:** the agent can overwrite a Skill the user relied on; one step back covers the common mistake without a version history.
- **Alternatives considered:**
  - No kept version: rejected by the user.
  - A full history: more storage and UI than the problem needs.

## Risks / Trade-offs

- [Injected instructions in a saved Skill] -> The approval shows the full text (D4). A Skill cannot grant approvals or tools beyond policy (client/skills Skill Tool Scope). Sharing needs the user (D11).
- [An agent-created Skill replaces a user's Skill] -> Updates are limited to agent-created Skills (D5). The provenance record is local (D6).
- [A new private Skill hides an org, group, shared or built-in Skill of the same name] -> A save whose name is in use by a usable Skill of another tier is refused (D5 and the collision rule in the spec).
- [Unattended runs create Skills] -> Refused (D10).
- [Secrets end up in a shared Skill] -> Secret text is refused at save (D8).
- [The user approves too many saves] -> One approval per save. The approval is short for small Skills. Accepted, because each save changes what the agent does later.
- [The model asks for a path anyway] -> The fixed sentence (D2) and the tool description are tested in the spike (task 1.1). If the model still asks, the tool description changes before group 2.
- [Users expect the new Skill in a running Run] -> The rule is stated in the approval result and the spec (D3).

## Migration Plan

Not applicable. The local table is created on first use.

## Acceptance

Task 3.2 (2026-11-11), Debug build in client/build/debug:

- `cmake --build build/debug`: exit 0.
- `ctest --test-dir build/debug -E "computer_win_tests|e2e_tests"`: 100% tests passed, 17 of 17 (total 275.57 sec). Includes win_tests, policy_tests, skills_tests, ui_tests, storage and runtime groups.
- `ShellFeed.StreamingDeltasKeepEarlierItems` failed once in an earlier full run (`EXPECT_EQ(rebuilt, 0u)`, got 1). It was not reproduced: 15 of 15 passed when repeated alone, and the full run passed 17 of 17. Cause not yet found; it stays open and is not a known flake.
- `openspec validate --all --strict`: 49 passed, 0 failed.
- Task 3.1 (desktop run) and 3.4 (archive) are not run and stay unchecked.

## Decided Questions (user, 2026-11-10)

- The save result shows the link to the Publish dialog in the Thread only; no inbox Question (D11).
- An update keeps one previous version for restore (D12).
- Roadmap number 41 [Managed] is in config.yaml.

## Spike Results (task 1.1, 2026-11-11)

Script: dev/scripts/skills-save-spike.mjs, organization LLM API (OPENAI_MODEL). The system prompt is the production one plus
the D2 sentence. The first turn offers only find_tools and ask_user, and skills.save is offered after find_tools loads it
(lazy loading as in client/src/runtime). Tool results are mocked. Tool description: the text of client/src/runtime tool
`skills.save`, as in the spike file.

| # | Prompt (abridged) | Calls | Saved | Path asked |
|---|---|---|---|---|
| 1 | make me a skill that turns my meeting notes into action items | find_tools, skills.save | yes | no |
| 2 | a reusable skill that reviews a pull request description | find_tools x2, ask_user | no | no (asked for the checklist content) |
| 3 | a skill that drafts a weekly status email from my task list | find_tools, skills.save | yes | no |
| 4 | a skill for checking that a CSV export has the right columns | find_tools, skills.save | yes | no |

Result: no path question in any of the four prompts; skills.save was called without a path in three. Prompt 2 asked for
the checklist content before drafting, which the spec does not forbid. No change to the tool description or the D2
sentence was needed, so group 2 proceeds with the text of the spike.

## Implementation Record (2026-11-11)

- Tests, before and after: win_tests 724 to 732 gtest cases, skills_tests 25 to 43, policy_tests 12 (one new table case), ui_tests 67. Full `ctest --test-dir build/test -E "computer_win_tests|e2e_tests"`: 17/17 before and after.
- Strict validation: `openspec validate --all --strict` passes (49 of 49 items).
- Decisions made while implementing (the design text above is unchanged where noted):
  - D2 and D10 are enforced by `ToolDef::attended_only`: the runtime does not offer or find the tool in Unattended runs or Child Runs, and a call from them fails with an error that names attended Runs.
  - D4 is carried by `ToolDef::approval_preview`: the Approval payload gets a `preview` with the files, the personal data findings and, for an update, the changed lines. The arguments in the payload carry the full text.
  - D8: the client secret rules are listed in D8 above (the registry scan is not implemented yet). The delta spec now says so.
  - D12: the kept version is a folder under `<data>/skills/previous/`, with Restore in the Skill editor (`skills.restore`, `skills.source` reports `previous`).
  - Refusals run in the tool precheck, before the approval, so a save that cannot succeed never reaches the user.
- Task 2.8 is done on top of the fix-feed-live-updates feed code: `publish_route_of` in client/src/shell/feed.cpp gives a saved Skill's Thread entry a link (`#/skills/edit/<name>`) to its Publish dialog. The offered tool set has no sharing tool.
- Not run: task 3.1 (desktop run, needs the user's OK). Task 3.4 (archive) is blocked: add-skills is 20/23 and not archived.
