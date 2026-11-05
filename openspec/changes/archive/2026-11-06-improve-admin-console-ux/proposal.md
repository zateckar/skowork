# Proposal

## Why

The product owner, the only platform-admin of a test deployment, asked on 2026-11-05 for a more intuitive admin console. A heuristic evaluation of the running console (Nielsen's ten heuristics, Hick's law) found these problems:

- **Navigation.** Up to thirteen views sit in one flat row of tabs with no grouping, and a view has no heading that says what it is for. A platform-admin scans ten tabs to find the right one (Hick's law, H6 recognition over recall).
- **Feedback.** The result of an action, such as "Revoked." or an API error, appears at the bottom of the view. In long views this is below the fold, so the admin does not see whether the action worked (H1 visibility of system status, H9 error recovery).
- **Button meaning.** Every form's last button is drawn as the green primary button, including destructive ones such as Deny on the deny list. Several destructive row buttons look like neutral buttons (H4 consistency, H5 error prevention).
- **Skill review.** The queue does not say how many versions wait, the open review does not show which row it belongs to and cannot be closed, Approve has the same weight as Reject, the list titled "Published Skills" also holds versions in review and rejected ones, and nothing explains the self-review rule before the admin runs into its error (H1, H4, H6).

## What Changes

- The console shows its views in a side navigation, grouped by task, and each view starts with a heading and a one-line purpose.
- The Skill review entry shows the number of versions waiting for review.
- Success notes appear as a dismissible notice near the top of the window, and action errors show both there and next to the action, so the result is visible wherever the admin has scrolled.
- Destructive buttons are drawn in the danger style everywhere, and primary buttons only for the main constructive action of a form.
- The Skill review view highlights the version under review, lets the admin close the review, makes Approve the primary action, lists every version under "All Skill versions" with its state, and tells the author of a version before they decide whether they can approve it (control-plane/skill-registry Org Review And Signing).

Out of scope:
- Changes to any API or to who may see which view.
- The policy editor, which improve-usability reworked.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `control-plane/admin-console`: Grouped Navigation, View Orientation, Pending Review Count, Action Feedback, Button Roles, Skill Review Workflow, Own Version Guidance.

## Impact

- **Roadmap:** 33, after use-idp-tokens-for-mcp. The Skill review part depends on add-skills, whose sole-reviewer rule it presents.
- **Code:** control-plane/admin (shell, shared UI pieces, stylesheet, Skill view, destructive buttons in other views), and the order of views returned by `/api/v1/admin/me`.
