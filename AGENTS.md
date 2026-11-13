# Skowork — working rules

This repository is **spec-driven with OpenSpec**. The specification is the source of truth and always comes before implementation.

## Rules

1. **Check the specs first, every time.** Before you answer a question about behaviour, design work or write code:
   - Run `openspec list --specs` to see what is accepted.
   - Run `openspec list` to see what is in flight.
   - Run `openspec show <id>` for the parts that apply, or read the files under `openspec/specs/` and `openspec/changes/`.
2. **Specs before code.** Any change in behaviour starts as a change in `openspec/changes/<change-id>/`, made of `proposal.md`, delta specs, `design.md` where needed, and `tasks.md`. Code is written only for an approved change, and only for what its specs cover.
3. **Everything is in the specification.** That covers requirements, decisions, constraints, risks, open questions and the roadmap. Nothing is documented outside `openspec/`:
   - Project-wide constraints, the glossary and the roadmap are in `openspec/config.yaml` under `context`.
   - Behaviour is in the specs.
   - Each change's rationale and decisions are in its `design.md`.
4. **Keep it clear and coherent.**
   - Use the glossary terms exactly.
   - One capability owns each concern. Other specs reference that capability and do not restate it.
   - When two specs disagree, fix the specs before you implement either one.
5. **Found a gap while implementing?** Stop. Update the change's delta specs, and `design.md` if it applies. Run validation, then continue.
6. **Validate before every commit:** `openspec validate --all --strict`.
7. **Archive after implementation.** Once every task is checked, run `openspec archive <change-id> --yes`. Then reread the merged specs under `openspec/specs/` to make sure they are still coherent.

## Commands

The CLI is `npx @fission-ai/openspec@latest <cmd>`, or `openspec <cmd>` if it is installed globally. Set `OPENSPEC_TELEMETRY=0`.

- `openspec list` / `openspec list --specs` — changes in flight / accepted capabilities
- `openspec show <change-or-spec> [--type change|spec]` — read one item
- `openspec validate --all --strict` — must pass
- `openspec archive <change-id> --yes` — merge a finished change into `openspec/specs/`
