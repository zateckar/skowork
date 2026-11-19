# Proposal

## Why

The product is called Skowork, but its executable, configuration keys, environment variables, file names, identifiers, documentation and specs still use the earlier name Workbench in about 1,200 places. The mixed names are confusing for users, operators and developers. The product spelling is also inconsistent: "SkoWork" appears beside "Skowork".

## What Changes

- Rename every occurrence of `Workbench` to `Skowork`, `workbench` to `skowork` and `WORKBENCH` to `SKOWORK` in tracked text files, including specs, documentation, code, tests, scripts, configuration, deployment files and CI workflows.
- Rename the product spelling `SkoWork` to `Skowork`, so the name is spelled the same way everywhere.
- Rename the two files whose names contain the old name: `client/src/app/workbench.ico` and `dev/scripts/workbench-pipe.ps1`.
- Remove the compatibility fallbacks that read the old names: the legacy registry policy key, the legacy machine configuration file and the legacy identifiers listed in the design. The client reads only the Skowork names.
- Resolve the places where a blind rename would duplicate an entry in a list or contradict a sentence, such as the executable allow-list and the machine-configuration requirement.

## Capabilities

This change has no spec deltas of its own (`skip_specs: true`). Spec text is renamed in place, and the one requirement whose meaning changes is written as a delta in the in-flight change `fix-desktop-machine-config` (client/control-channel, Machine Configuration Names): it keeps only the Skowork registry key and `skowork.machine.json`, as decided in `design.md` D2.

- **Specs:** name-only changes to the text of every capability. No other requirement changes its behaviour.

## Out of scope

- Any other behaviour change. The rename does not change policy, permissions, endpoints, telemetry semantics or data formats.
- Data and configuration migration. Existing installations and deployments must be migrated by their operators; see `design.md`.
- Changes to historical archive content beyond the name, and no change to Git history.
- Choosing a new product name or brand styling.

## Impact

- **Code:** client (CMake targets, executables, machine configuration, identifiers, UI strings, tests and tooling), control plane, dev tooling, deployment files and CI workflows.
- **Specs:** every spec, change and archived change under `openspec/` is renamed in its text. Only the machine-configuration requirement changes meaning.
- **Operations:** existing deployments need the operator actions listed in `design.md` (registry key, configuration file, environment variables, database file name, identity-provider client ID, telemetry attribute names, local data folder). They are not automated here.
- **Data:** no automated migration. The rename changes file and folder names that the client creates, so existing local data must be moved by hand or it appears missing.
