## 1. Validation spike

- [x] 1.1 Check that the model finds `skills.save` through `find_tools` and calls it without asking for a path, using the real organization LLM API from a local dev script (outside the product code; the key stays in .env). Verify: at least 3 prompts ("make me a skill that ...") recorded in design.md, and no path question in any of them. If the model asks for a path, change the tool description and the system prompt sentence (D2) before group 2.

## 2. Client tool and rules

- [x] 2.1 `skills.save` arguments, validation through the Skill editor's rules, the secret check, the file rules and the save path (D1, D7, D9). Verify: unit tests in client/tests/skills/skills_authoring_test.cpp for create, invalid frontmatter, a secret in the instructions (refused, value not in the message), binary file, unsafe path and the file limits
- [x] 2.2 Name collisions and updates (D5). Verify: unit tests for a name used by an org, group, shared or built-in Skill (refused, naming the tier), a name of a private Skill the user wrote (refused, file unchanged), update with a higher version (replaced), update with equal or lower version (refused), update of a user's Skill (refused)
- [x] 2.3 Provenance record in the local store (D6). Verify: unit tests in client/tests/storage/storage_test.cpp for insert on create, update time on update, removal when the editor deletes the Skill, and that no Skill text is stored
- [x] 2.4 Approval payload and personal data findings (D4, D8). Verify: unit tests in client/tests/policy/policy_core_test.cpp that a skills.save call declares write-local and is decided approve by default; and in client/tests/skills/skills_authoring_test.cpp that the approval payload lists the full text, the files, the findings and, for an update, the changed lines
- [x] 2.5 Offer rules (D2, D10). Verify: runtime tests in client/tests/runtime/runtime_test.cpp that `skills.save` is absent from unattended and Child Runs (a call returns an error, nothing is stored), present in top-level Runs that are not Unattended runs, and that `find_tools` returns it for "create a skill"
- [x] 2.6 Availability (D3). Verify: runtime tests in client/tests/runtime/runtime_test.cpp that a Run started after the save lists the new Skill, that a Run executing during the save keeps its list, and that an explicit `/name` after the save resolves the new Skill
- [x] 2.7 Stable prompt (D2). Verify: a runtime test that two attended Runs in one Thread send byte-identical system prompts, and that the fixed sentence is in both
- [x] 2.8 Sharing hand-off (D11). Verify: a unit test that the tool set offered to the agent has no sharing tool, and that the saved Skill's Thread entry carries the link to its Publish dialog
- [x] 2.9 Previous version and restore (D12). Verify: unit tests in client/tests/skills/skills_authoring_test.cpp that an update keeps the replaced SKILL.md and files, a second update keeps only the latest replaced version, Restore swaps current and kept, and deleting the Skill deletes the kept version; a shell test that the Skill editor shows Restore previous version only for a Skill with a kept version

## 3. Acceptance

- [ ] 3.1 Desktop run of the whole flow: ask for a Skill in chat, check that no path is asked and the approval shows the full text, check the Skill in the Skills section, start a new Run that uses it, and check that the agent offers sharing and does not share. Needs the user's explicit approval before the run (desktop input). Verify: results recorded in design.md under Acceptance
- [x] 3.2 Run `cmake --build --preset test` in client/ and the tests of groups 2.1 to 2.8. Verify: the build and the named tests pass; record the output summary in design.md under Acceptance. Tests that already passed are not run again.
- [x] 3.3 Run `openspec validate --all --strict`. Verify: no errors and no warnings
- [ ] 3.4 After all tasks are checked, run `openspec archive add-agent-created-skills --yes` only after add-skills is archived, and reread the merged specs under openspec/specs/client/. Verify: archive succeeds and the merged specs are coherent
