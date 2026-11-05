// Admin console authorization (control-plane/admin-console Admin Roles; design D6): the views each role may open,
// the gate in front of every admin API, and the audit of refused actions.
import type { NextFunction, Request, Response } from 'express';
import { sendError } from '../http.ts';
import type { Audit } from '../identity/audit.ts';
import type { AdminRole } from '../identity/identity.ts';

/**
 * Console views and the roles that may open them. The server checks the same table on each view's API. The order is
 * the console's grouped navigation order (improve-admin-console-ux D1).
 */
export const VIEWS = {
  dashboards: ['platform-admin', 'security-auditor'],
  identity: ['platform-admin'],
  fleet: ['platform-admin', 'security-auditor'],
  policy: ['platform-admin', 'security-auditor', 'group-owner'],
  models: ['platform-admin'],
  connections: ['platform-admin'],
  mcp: ['platform-admin'],
  skills: ['platform-admin', 'skill-reviewer'],
  knowledge: ['platform-admin'],
  remote: ['platform-admin'],
  audit: ['security-auditor'],
  individual: ['security-auditor'],
  'access-records': ['security-auditor', 'works-council-reviewer'],
} as const satisfies Record<string, readonly AdminRole[]>;
export type View = keyof typeof VIEWS;

export const viewsFor = (roles: readonly AdminRole[]): View[] =>
  (Object.keys(VIEWS) as View[]).filter((v) => (VIEWS[v] as readonly AdminRole[]).some((r) => roles.includes(r)));

/** Express middleware: the signed-in admin may open `view` (exact roles; platform-admin is not implied). */
export function requireView(view: View) {
  return (req: Request, res: Response, next: NextFunction): void => {
    if (viewsFor(req.user?.adminRoles ?? []).includes(view)) return next();
    sendError(res, 403, 'FORBIDDEN', 'Your administrator roles do not include this view.');
  };
}

/**
 * In front of every admin API: users without an admin role are refused, and every refusal (403) under the admin API
 * is audited with the method and path, without the query string.
 */
export function adminGate(audit: Audit) {
  return (req: Request, res: Response, next: NextFunction): void => {
    const u = req.user!;
    const path = req.originalUrl.split('?')[0]!;
    res.on('finish', () => {
      if (res.statusCode !== 403) return;
      audit.write({
        category: 'admin', action: 'admin.refused', outcome: 'refused', subject: u.subject, userName: u.userName,
        reason: u.adminRoles.length ? 'role does not allow this action' : 'no administrator role', details: { method: req.method, path },
      });
    });
    if (u.adminRoles.length === 0) return sendError(res, 403, 'FORBIDDEN', 'An administrator role is required.');
    next();
  };
}
