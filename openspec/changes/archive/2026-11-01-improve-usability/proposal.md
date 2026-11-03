# Proposal

## Why

The product owner ran the whole stack on a workstation on 2026-10-31 (control plane, admin console and a development client, signed in through the organization Keycloak) and used it as a user and as an admin. Several things did not work for a person, even where the code behind them exists:

- The admin console layout breaks at ordinary window widths. Five cards are squeezed into one row and table content overflows its card.
- Policies can be edited only as raw JSON. An admin who wants to switch on computer use or allow a host must know key names such as policy.tools.computer.mode.
- When policy switches a capability off (computer use, the shell), the agent searches for it, finds nothing and tells the user it has no such tool. Nobody learns that the capability exists and that an administrator can enable it.
- The desktop app shows only the policy version and Groups, although client/policy-enforcement Policy Visibility To User requires a readable summary. Folder grants (User Folder Grants) have no user interface at all.
- An audit of every user-facing requirement against the desktop and admin UIs found further gaps where behaviour is specified and implemented but cannot be reached: renaming and deleting folders, editing queued messages, per-Run usage totals, withdrawing a published Skill, and the item exclusion of the knowledge settings.

## What Changes

- Admin console: a layout that works from 1280 px wide, labelled inputs, accessible names for icon buttons, and confirmation before destructive actions.
- Admin console: a structured policy editor with typed controls for each policy key, kept in step with the raw JSON view.
- Client: when a tool search matches tools that policy withholds, the agent is told which capability is withheld and which policy key controls it, so it can explain this to the user.
- Client: a Policy page in Settings with the full effective policy summary, and a Files section to add and revoke folder grants.
- Client: user interface for already-specified behaviour that had none (see Why).

Out of scope:
- LLM connection management (add-llm-connections).
- Knowledge import, curation and sharing (add-knowledge-sharing).
- Any change to policy decisions, key semantics or defaults.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/policy-enforcement`: Withheld Tool Explanation.
- `client/work-inbox`: Policy Page and Files Settings.
- `control-plane/admin-console`: Structured Policy Editor, Console Usability.

## Impact

- **Roadmap:** 14, after add-meeting-capture. It depends on add-policy-and-sandbox, add-work-inbox, add-native-ui and add-telemetry-and-admin, whose capabilities it extends, and is archived after them.
- **Code:** control-plane/admin (React views and stylesheet), client/src/runtime (tool search), client/src/policy, client/src/shell (Settings pages).
- **Forward references:** add-llm-connections (15) adds model pickers to the Models view; add-knowledge-sharing (16) extends the knowledge settings.
