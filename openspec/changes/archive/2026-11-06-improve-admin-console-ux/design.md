# Design

## Context

The console is a React single-page app in control-plane/admin. Views are chosen by role on the server (`/api/v1/admin/me` returns `views`) and addressed as `#/<view>`. Shared pieces live in `ui.tsx`. The end-to-end tests drive it through `data-testid` attributes, which this change keeps.

## Decisions

### D1. Side navigation grouped by task
- The groups and their order are fixed in the shell. The server keeps deciding which views a role gets, and returns them in the shell's grouped order, so the order is the same in both.
- The shell fetches the review queue once for the badge when the admin may see Skill review, and again when the Skill view reports a decision.
- The navigation is a column of about 220 px. Below 1100 px it becomes a horizontal row above the view, so the 1280 px rule of Console Usability still holds with room for the cards.
- Alternatives considered: a top bar with drop-down menus. They hide the views behind a click, and the console has few enough views to show them all.

### D2. Notices
- `Note` renders a fixed notice at the top centre of the window with a close button, `role="status"`. A newer note replaces the older one. It stays until it is dismissed or the next action starts, so a slow reader and the end-to-end tests still find it.
- `Err` keeps its place next to the action. A view-level action error is also raised as a notice with `role="alert"`.
- Alternatives considered: notices that hide themselves after a few seconds. They disappear before an admin who looked away reads them (H1).

### D3. Button roles
- The stylesheet no longer makes every form's last button primary by position alone. A destructive submit button carries `danger`, which wins over the position rule. Row buttons that revoke, deny, lift, retire or remove carry `danger` too.

### D4. Skill review
- The detail response carries `approval` (`review`, `self-approval` or `self-review-blocked`) from the registry (add-skills D6 Sole reviewer). The view shows a notice for the two self cases, labels the comment as required to approve in the self-approval case, and disables Approve when it is blocked.
- Opening a review scrolls it into view. Its card has a Close button, and its queue row is highlighted.

## Risks / Trade-offs

- [Risk] End-to-end tests compare the navigation order with a fixed list. -> Mitigation: the lists in `admin-e2e.test.ts` follow the grouped order.
