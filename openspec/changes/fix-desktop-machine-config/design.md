# Design

## Context

The README and add-client-foundation D10 promise `HKLM\SOFTWARE\Policies\Workbench` first when `ControlPlaneUrl` exists, then `workbench.machine.json` beside `Workbench.exe`. `exe_dir()` already uses `GetModuleFileNameW`, and production startup calls that loader. `load_machine_config_file` currently reads binary bytes directly into the UTF-8 JSON parser. Missing/open failures share a generic message, while parse and validation errors name only the basename. The user reports Windows Notepad and the generic unconfigured banner; that alone cannot identify the deployment's precise failure.

## Decisions

### D1: Normalize BOM-marked UTF-16 at the file boundary

Decode little- and big-endian UTF-16 into code units, strictly convert through Windows UTF-8 conversion, then use the existing JSON parser. Keep UTF-8 with optional BOM supported. Reject odd byte counts, invalid surrogate sequences and unsupported encodings. Do not change parsing of network JSON. Alternatives: requiring users to resave UTF-8 leaves a common Windows editor format unsupported; encoding guessing makes malformed input ambiguous. No new library is needed. Processing is linear in this small startup configuration; no background work or material baseline hardware cost is added.

### D2: Preserve source selection and expose actionable errors

Keep registry selection and the exact filename unchanged. Error messages include the full attempted path and the reason, with file keys named for incomplete JSON. If that exact path is missing, check only for the same path plus `.txt` to suggest a rename. Do not load it, search the working directory, or fall back after invalid registry settings. Distinguish read failures from absence. Alternatives: accepting arbitrary JSON files or changing registry fallback would weaken the established deployment contract; generic errors obscure a wrong filename or launch location.

### D3: Exercise the production loader in isolated tests

A dedicated Windows test executable links the real channel loader, path resolution, HTTP URL validation and loopback validation. A link-time registry reader double supplies deterministic absent/valid/invalid registry values without modifying machine or user registry. Copy that test executable into a unique temporary Unicode directory and run its probe mode with a different working directory and adjacent JSON; no development-only config argument is used. Test the release preset with development endpoints disabled. Alternatives: testing only an explicit file path misses source discovery, and launching the resident production app would modify the user's profile and registration.

## Risks

- [The user's precise production cause remains unknown] -> Reproduce the supported encoding failures, verify the README example, and expose the attempted path and extra extension rather than claim a deployment-specific diagnosis.
- [Permissive conversion accepts malformed Unicode] -> Reject truncated UTF-16 and test invalid surrogate sequences and malformed JSON.
- [Tests alter real configuration or user state] -> Use a registry double and unique temporary executable directories; never overwrite a real adjacent configuration.
- [Two changes add to client/control-channel] -> Keep new requirement names separate from add-client-foundation Control-Plane Discovery and reread the merged spec after archive.

## Validation

Record reproduction and final release-build/test results here after execution.
