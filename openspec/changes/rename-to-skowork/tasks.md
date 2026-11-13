# Tasks

## 1. Specification
- [x] 1.1 Write the proposal, design and tasks for the rename, including the operator actions in design D5.
- [x] 1.2 Rewrite the machine configuration names requirement in `fix-desktop-machine-config` (its delta spec, design D4 and task 2.2) so that it names only the Skowork registry key and `skowork.machine.json`.

## 2. Implementation
- [x] 2.1 Rename `client/src/app/workbench.ico` to `skowork.ico` and `dev/scripts/workbench-pipe.ps1` to `skowork-pipe.ps1`.
- [x] 2.2 Replace `Workbench`, `workbench`, `WORKBENCH` and `SkoWork` in all tracked text files (specs, changes, archive, code, tests, scripts, configuration, workflows, documentation, presentation). Binary files are not changed.
- [x] 2.3 Remove the legacy fallbacks in the machine configuration loader, its tests and its diagnostics. Remove duplicate list entries (design D4) and reword sentences that describe the old name as a compatibility fallback.

## 3. Validation
- [x] 3.1 Search tracked text files for `Workbench`, `workbench` and `WORKBENCH` and confirm no occurrence remains.
- [x] 3.2 Run `openspec validate --all --strict` and confirm it passes.
- [x] 3.3 Run the control-plane test suite and confirm it passes.
- [ ] 3.4 Build the client and run its tests where the local toolchain allows, and record the result in design.md. Record anything not run.

## 4. Archive
- [ ] 4.1 Run `openspec archive rename-to-skowork --yes` once every task is checked, then reread the merged specs under `openspec/specs/` for coherence.
