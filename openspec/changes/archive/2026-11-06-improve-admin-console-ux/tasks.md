## 1. Admin console

- [x] 1.1 Grouped side navigation with view headings and the review count badge, and the grouped view order from `/api/v1/admin/me` (D1). Verify in the browser as platform-admin, and with the end-to-end navigation test for every role
- [x] 1.2 Notices for success notes and action errors (D2). Verify in the browser that a revocation at the bottom of the Skill view shows its notice at the top
- [x] 1.3 Button roles (D3). Verify in the browser that Deny and the row revoke and remove buttons are in the danger style
- [x] 1.4 Skill review workflow (D4). Verify in the browser as the sole platform-admin approving their own import with a comment, and with the end-to-end Skill review test

## 2. Acceptance

- [x] 2.1 Run `openspec validate --all --strict`
