# Proposal

## Why

A production user followed the README, created the adjacent JSON configuration in Windows Notepad, and still saw "Skowork is not configured". The loader assumes byte-oriented UTF-8, has no direct regression coverage, and does not identify the actual file path on failure; the exact encoding and filename used in that deployment are not yet known.

## What Changes

- Accept the documented adjacent JSON configuration in UTF-8 (with or without BOM) and BOM-marked UTF-16 LE/BE, as Windows editors can save it.
- Report the exact configuration path and distinguish missing, unreadable, malformed and incomplete configuration. Detect the common extra `.txt` extension and explain how to rename it.
- Verify executable-relative discovery from a different working directory and Unicode installation path, preserving registry precedence, defaults and production URL validation.

## Capabilities

### New Capabilities
- `client/control-channel`: Machine Configuration Sources, Machine Configuration Encoding and Machine Configuration Diagnostics. This capability already exists in add-client-foundation but has not yet been archived; this change adds only these requirements, leaving Control-Plane Discovery there.

### Modified Capabilities
- None.

## Out of scope

Changing identity, deployment trust boundaries, registry precedence, settings keys, user-editable settings, live configuration reload, or the control plane.

## Impact

- **Roadmap:** 35 [Hardening], after improve-deployment-guidance; depends on add-client-foundation D10 and its client/identity and client/control-channel requirements.
- **Code:** client machine-configuration loading and isolated Windows tests; no new dependency.
- **Data:** no migration and no changes to existing user configuration or registry entries.
