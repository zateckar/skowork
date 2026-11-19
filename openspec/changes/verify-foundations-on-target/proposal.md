# Proposal

## Why

The four foundation changes (add-client-foundation, add-work-inbox, add-knowledge-graph, add-agent-runtime) and later add-native-ui and add-skills are implemented and their behaviour is covered by local tests. Each one still has open checks that cannot run on the development workstation. They need an organization laptop, the baseline laptop, the organization Keycloak or Entra tenant, the real LLM gateway, the corporate proxy and VPN, a second Windows account, desktop input, a signing certificate, or a push to `main`. Because those checks stay open, the foundations cannot be archived. Nine finished changes that modify the foundations' requirements cannot be archived either.

## What Changes

- Move every check that needs target hardware, real identity providers, the real gateway, other people, publishing or desktop input out of the foundation changes into this change's `tasks.md`. Keep the original wording and verification criteria.
- In each foundation's `tasks.md`, mark each moved task with a pointer to its task here, so the foundation can be archived.
- Record in the foundations what was verified locally on 2026-11-19 and what still waits for this change.

## Capabilities

This change has no spec deltas (`skip_specs: true`). It verifies requirements already in `openspec/specs/` once the foundations are archived. A failed check here starts a new change that fixes the owning capability.

## Out of scope

- New behaviour or changed requirements.
- Checks owned by a change that stays open for other reasons.

## Impact

- **Specs:** none.
- **Changes:** add-client-foundation, add-work-inbox, add-knowledge-graph and add-agent-runtime get pointers to this change in their `tasks.md`.
- **Code:** none expected. Any defect found becomes its own change.
