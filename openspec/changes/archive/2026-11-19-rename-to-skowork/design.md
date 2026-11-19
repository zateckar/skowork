# Design

## D1: The product name is Skowork

- Every occurrence of `Workbench` becomes `Skowork`, `workbench` becomes `skowork`, and `WORKBENCH` becomes `SKOWORK`.
- The product spelling `SkoWork` becomes `Skowork`. This makes one spelling of the product name everywhere, as the user requested (2026-11-13). The Windows registry and file names are case-insensitive, so the rename of existing `SkoWork` names does not change how Windows resolves them.
- The glossary in `openspec/config.yaml` follows this spelling.

## D2: No compatibility with the old names

The user chose "Rename everything" (2026-11-13) after being told that the existing deployment's settings and data would break unless migrated. Therefore:

- `improve-skowork-and-knowledge-control` D2 (keep Workbench for internal contracts) is superseded. The internal contracts are renamed too.
- The `Product Rename Compatibility` requirement in `improve-skowork-and-knowledge-control` (existing data, credentials, settings and integration identifiers keep working; a shortcut to the former executable opens the client) is removed from that change's delta, because it promises the opposite. The change is not archived yet, so this is a removal from its delta, not a MODIFIED or REMOVED entry in the merged specs.
- `fix-desktop-machine-config` D4 (read Workbench names as a fallback) is superseded. The client reads the Skowork registry key `HKLM\SOFTWARE\Policies\Skowork` and `skowork.machine.json` only. Its delta spec and its tasks 2.2 are rewritten accordingly.
- No code reads a `Workbench`-named registry key, file, environment variable or database file.

## D3: Legacy launcher

`Workbench-legacy.exe` is renamed to `Skowork-legacy.exe`. It is a small launcher that starts `Skowork.exe`. It is shipped only under its own name. Earlier packaging copied it as the product executable, which would overwrite `Skowork.exe`, so that copy is removed. Shortcuts to the former `Workbench.exe` no longer resolve to anything, so they must be recreated or pointed at `Skowork-legacy.exe`. Whether to keep this launcher at all is a separate product decision and is not decided here. Decided 2026-11-17 in improve-skowork-and-knowledge-control D1: the launcher is removed.

## D4: Duplicates from merged names

Where a list or sentence held both the old name and the Skowork name, the duplicate entry is removed rather than repeated. The executable allow-list in `client/src/computer/service.h` is one example.

## D5: Operator actions after the rename

These names are outside the repository and must be changed by the operator. The rename does not change them automatically.

| Item | Before | After |
| --- | --- | --- |
| Machine registry key | `HKLM\SOFTWARE\Policies\Workbench` (and the Skowork key) | `HKLM\SOFTWARE\Policies\Skowork` |
| Machine configuration file | `workbench.machine.json` | `skowork.machine.json` |
| Environment variables | `WORKBENCH_*` | `SKOWORK_*` |
| Control-plane database file | `workbench.db` | `skowork.db` |
| Identity-provider client ID | `workbench-desktop` | `skowork-desktop` (the client ID in the organization's Keycloak must be created or renamed to match) |
| Telemetry attribute names | `workbench.channel`, `workbench.installation_id`, the OpenTelemetry scope `workbench.client` | `skowork.*` (dashboards and collectors must follow) |
| Desktop executable and zip | `Workbench.exe`, `Workbench-*.zip` | `Skowork.exe`, `Skowork-*.zip` |
| Windows app user-model ID | `Workbench.Client` | `Skowork.Client` (existing taskbar grouping and notification registration reset) |
| Local data folder | the data root under the Workbench name | the Skowork name; existing data must be moved, or the client starts empty |

## D6: Verification

- No occurrence of `Workbench`, `workbench` or `WORKBENCH` remains in tracked text files, except where the text is the URL of an external service or a third-party name that cannot change (none are expected).
- `openspec validate --all --strict` passes.
- The control-plane test suite passes.
- The client build and tests are run where the local toolchain allows. Anything not run is recorded in this section.

## Verification record

- 2026-11-19, name search (`git grep -i workbench`, outside this change): what remains is deliberate. The control-plane migration renames the old `workbench_group` columns of existing databases, and its tests create them. A policy test stores a bundle under the old `workbench-policy+jwt` type to check it is issued again. A design note in improve-skowork-and-knowledge-control names the DPAPI purpose of an old recording. Two leftovers were fixed: a test temporary folder name, and a task note that named `WORKBENCH_DEV_PROXY` for `SKOWORK_DEV_PROXY`.
- 2026-11-19, `openspec validate --all --strict` passes.
- 2026-11-19, control plane: `npm run check` and `npm test` pass (338 tests).
- 2026-11-19, client: the debug and release presets build. `ctest --preset debug -LE "e2e|desktop"` passes every suite except `win_tests`, where 945 of 957 tests pass, 10 are skipped (opt-in measurements and child-process helpers) and 2 fail. Unrelated failures fixed in the same pass: a stale Settings golden image, a feed status line regression, a speech Repair that gave up on a briefly locked folder, a test that freed a gate a Run still waited on, and a fuzz smoke over its time limit. Still intermittent, not caused by the rename and already failing before these edits: `ShellKnowledge` tests that click a Knowledge row while the list reloads, after which the reader sometimes does not open (for example WorkspaceTraversesEvidenceRevisionsAndReviews, SaveMailAndChatAsNote, KnowledgeViewImportNotesSearchCurateAndShare). Each passes when run on its own. The parser fuzz smokes pass. The release `machine_config_tests` and string checks pass, both executable manifests pass, and the core install is 28.89 MB against the 30 MB budget.
- Not run: the e2e and desktop-input suites (they need an identity-provider sign-in or take over the desktop), signing, and launching the packaged client.
