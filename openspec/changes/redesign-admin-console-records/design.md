# Design

## Context

The console is a React single-page app in control-plane/admin, reworked by improve-admin-console-ux into a grouped side navigation with notices. Each view still pairs every record list with an open form. The end-to-end test drives the console through `data-testid` attributes and fills forms by field name.

## Decisions

### D1. Shared building blocks
- `Records`: a card with a title, a filter box, the count ("3 of 12") and the primary action, over a table. The filter matches the text of each row. An empty table shows a sentence and the add action.
- `Dialog`: the native `<dialog>` element opened with `showModal()`, which gives the backdrop, the focus trap, Escape and the accessibility role. Its body is a `Form` whose submit runs the action. On success it closes and raises the notice; on failure it keeps its values and shows the error at its foot. Forms keep their `data-testid` and field names, so the end-to-end test fills them as before after opening the dialog.
- `ask()`: a promise-based confirmation dialog (title, consequence, action label, optional reason) that replaces `window.confirm`. It resolves to the reason, or null when cancelled.
- `Picker`: a combobox built from a text input with a filtered list below it (ARIA combobox pattern: arrow keys, Enter, Escape). Single and multiple selection; multiple shows chips. The submitted value is kept in a hidden input with the field's `name`, comma-separated for several. A typed value that is not in the list is accepted where `allowNew` is set and shown as new.
- `Tabs`: a tab list whose selection is written to the address as `#/<view>/<tab>`.

### D2. Directory
- `GET /api/v1/admin/identity/directory` returns `{ users: [{ name, sources, bootstrap }], idp_roles: [] }`, sorted. Sources: installations, direct members, administrator roles, Group owners, the deny list and `WORKBENCH_BOOTSTRAP_ADMINS`. Roles: the `idp_roles` recorded on installations and the mapped roles. It needs an administrator role, which the admin gate already demands.
- Groups for pickers come from `GET /api/v1/admin/policy/groups` (Every Known Group Is Listed), where the admin may open the policy view; elsewhere the field accepts typed Group names.
- Alternatives considered: querying the identity provider's user directory. Workbench trusts the identity provider for sign-in only and has no directory credentials; the users the control plane has seen are the ones an admin manages.

### D3. View by view
- **Identity:** tabs. Administrators: one row per user with their roles as chips; Grant opens user picker and role checkboxes with what each role may do; Edit changes the checkboxes (grants and revokes the difference); bootstrap administrators are listed as configured and cannot be edited here. Group members: one row per Group with its members as chips; Add members opens Group picker (new allowed) and users picker; Edit members adds and removes the difference. Role mappings: rows; Add opens identity-provider role picker (new allowed), Group picker and reason. Service principals: rows; Register opens name, client id and APIs; Retire asks. Deny list: rows; Deny access opens kind, user picker or installation picker, reason; Lift asks.
- **Fleet:** filter toolbar (search, version, Group and state as selects built from the data), installations table, row actions Revoke installation and Revoke user everywhere, both asking for a reason. Minimum versions: table per channel; Set minimum opens channel and version.
- **Policies:** the scope list gets a New Group button (dialog: name, description, owners picker) and the selected Group an Edit Group action. The scope header shows the version in force, rollout state and the actions Publish draft (dialog: reason, rollout to everyone, a pilot Group picker or a percentage), Promote, Cancel rollout and Roll back. Versions and adoption are tables. The draft editor stays on the page with Save draft (reason in a dialog) and Review changes (the diff in a dialog). Simulation: a user picker or Group picker and the result table.
- **Models & quotas:** a Group picker, then the effective role mapping and budgets as read-only tables that mark inherited values; Edit opens the form in a dialog with publish now.
- **LLM connections:** table; Add connection and Edit open dialogs; row actions Test, Enable or Disable, Delete (asks, naming the role mappings that use it); a row expands to its models table with Edit per model in a dialog.
- **MCP catalog:** table with Groups as chips and the number of tools waiting for a decision; Add server and Edit open dialogs with owner (user picker) and Groups (Group picker); Tools opens a dialog to include or exclude each discovered tool.
- **Skill review:** the queue and all versions as tables; Review opens a wide dialog with the scan, diff and decision; Import opens a dialog; Revoke asks for the reason.
- **Team knowledge:** Group filter and table; Remove asks for the reason.
- **Remote tasks:** tabs. Dispatch rules: Add opens issuer kind and an issuer picker that follows the kind (users, Groups, administrator roles, service principals), target kind and target picker. Rate limits: Set limit opens an issuer picker of the service principals and users, and the rate; Edit per row. Callbacks: Add prefix.
- **Audit log:** a filter toolbar; a row opens its details in a dialog. Searching a user asks for the reason in the toolbar.
- **Individual access** stays a query form with a user picker, because the reason is part of the query itself.

### D4. Visual system
- Calm and dense, for an admin who spends long sessions in it: the Škoda emerald `#0E3A2F` carries the navigation and primary actions, the electric green `#78FAAE` marks the current view and focus only, neutrals are cool greys (`#F4F6F5` canvas, `#FFFFFF` surface, `#DCE2DF` lines, `#1A2420` text) and a muted red `#B42318` is reserved for destructive actions. Dark mode inverts the neutrals and keeps emerald for primary surfaces.
- One typeface: the system UI face (Segoe UI on Windows), with a scale of 12/13/14/16/20 px; table text 13 px, headings 16 and 20 px.
- Tables carry the views: sticky headers, 36 px rows, actions right-aligned and revealed in muted text, the destructive one last.
- Rejected default: identical rounded cards with soft shadows for every section. Sections are plain surfaces with a hairline border, and only dialogs float with a shadow, so the one thing that floats is the thing that needs a decision.

## Risks / Trade-offs

- [Risk] The end-to-end test fills forms that now live in dialogs. -> Mitigation: each step first opens the dialog by its button's `data-testid`; form ids and field names stay.
- [Risk] A custom combobox can be less accessible than a native control. -> Mitigation: it follows the ARIA combobox pattern and keeps a labelled text input, so typing always works.
