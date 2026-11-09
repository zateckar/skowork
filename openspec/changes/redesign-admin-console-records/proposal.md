# Proposal

## Why

After improve-admin-console-ux, the product owner judged on 2026-11-06 that the main problem had not been touched: the forms. Every view stacks its records and an always-open add form on the same page. The Identity view shows five lists and five add forms at once, the MCP catalog edits a server's Groups in a text box inside the table row, and the Fleet view puts a revoke form under the installation table. Fields that name something the control plane already knows (a user, a Group, an identity-provider role, a service principal, a model) are free text, so the admin has to remember exact spellings, such as the owner of a Group (H6 recognition over recall, H5 error prevention, H8 minimalist design).

## What Changes

- **Records first.** Each view shows its records as a table with a toolbar: a filter, the count and one primary action to add a record. Forms are not shown on the page.
- **Dialogs.** Adding and editing a record opens a dialog with its form. The dialog is labelled, traps focus, closes with Escape or Cancel, and shows the error of a failed save inside it, so nothing is lost.
- **Pickers.** Fields that name a known user, Group, identity-provider role, administrator role, service principal or catalog entry offer the known values as a searchable list, and fields that take several of them, such as the owners of a Group, take them as removable chips. Where a new value is valid, such as a Group without a policy yet, it can still be typed.
- **Confirmation dialogs.** Destructive actions ask in a dialog that names the record and the consequence, and that takes the reason where the action records one, instead of the browser's confirm box.
- **Sections.** The Identity and Remote tasks views split their record sets into tabs.
- **Known users and roles.** control-plane/identity lists the users and identity-provider roles it knows, for the pickers.
- A deliberate, calm visual system for a dense administrative tool, in light and dark.

Out of scope:
- New administrative functions. Every action the console offers today stays, with the same API.
- The structured policy editor, which stays an editor on the page.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `control-plane/admin-console`: Records First Views, Record Dialogs, Pickers For Known Values, Confirmation Dialogs, View Sections.
- `control-plane/identity`: Admin Directory.

## Impact

- **Roadmap:** 34, after improve-admin-console-ux.
- **Code:** control-plane/admin (every view, the shared UI pieces, the stylesheet), control-plane/src/identity (directory endpoint), and the admin end-to-end test, which now opens dialogs before it fills them.
