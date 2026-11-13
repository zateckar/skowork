// Identity endpoints: whoami for clients, and the admin API for the deny list, role mappings, Group membership and
// admin roles.
import { Router } from 'express';
import type { Db } from '../db.ts';
import { nowIso, tx } from '../db.ts';
import { sendError, str } from '../http.ts';
import type { Audit } from './audit.ts';
import { ADMIN_ROLES, type AdminRole, requireRole } from './identity.ts';

export interface DenyHooks {
  /** Called after a deny-list entry is added, so the fleet registry can push revocations. */
  onDenied(kind: 'user' | 'installation', value: string, reason: string): void;
  /** Called after the role mappings change; they decide everybody's Groups (policy-service Notification Scope). */
  onMappingChanged?(): void;
  /** Called after a direct Group assignment of one user changes. */
  onAssignmentChanged?(userName: string): void;
  /** SKOWORK_BOOTSTRAP_ADMINS, listed in the directory. */
  bootstrapAdmins?: readonly string[];
}

export function identityRoutes(): Router {
  const r = Router();
  r.get('/api/v1/identity/whoami', (req, res) => {
    const u = req.user!;
    res.json({
      issuer: u.issuer,
      subject: u.subject,
      user_name: u.userName,
      email: u.email,
      display_name: u.displayName,
      groups: u.groups,
      admin_roles: u.adminRoles,
      token_expires_at: u.tokenExpiresAt,
    });
  });
  return r;
}

export function identityAdminRoutes(db: Db, audit: Audit, hooks: DenyHooks): Router {
  const r = Router();
  const record = (req: Parameters<Parameters<typeof r.get>[1]>[0], action: string, details: unknown) =>
    audit.write({ category: 'admin', action, outcome: 'ok', subject: req.user!.subject, userName: req.user!.userName, details });

  // Deny list
  r.get('/deny-list', requireRole('security-auditor'), (_req, res) => {
    res.json(db.prepare('SELECT * FROM deny_list ORDER BY id DESC').all());
  });
  r.post('/deny-list', requireRole('platform-admin'), (req, res) => {
    const kind = str(req.body?.kind);
    const value = str(req.body?.value)?.trim();
    const reason = str(req.body?.reason)?.trim() || 'denied by administrator';
    if ((kind !== 'user' && kind !== 'installation') || !value) {
      sendError(res, 400, 'INVALID_REQUEST', 'kind (user|installation) and value are required.');
      return;
    }
    addDenied(db, kind, value, reason, req.user!.userName);
    record(req, 'identity.deny-list.add', { kind, value, reason });
    hooks.onDenied(kind, value, reason);
    res.json({ kind, value, denied: true });
  });
  r.delete('/deny-list/:id', requireRole('platform-admin'), (req, res) => {
    const row = db.prepare('DELETE FROM deny_list WHERE id = ? RETURNING kind, value').get(Number(req.params.id));
    if (!row) {
      sendError(res, 404, 'NOT_FOUND', 'No such deny-list entry.');
      return;
    }
    // Lifting an installation entry also lifts the installation's revocation.
    const { kind, value } = row as { kind: string; value: string };
    if (kind === 'installation') db.prepare('UPDATE installations SET revoked_at = NULL WHERE id = ?').run(value);
    record(req, 'identity.deny-list.remove', row);
    res.json({ removed: true });
  });

  // Role mappings: identity-provider role -> Skowork Group (many-to-many)
  r.get('/role-mappings', requireRole('platform-admin'), (_req, res) => {
    res.json(db.prepare('SELECT idp_role, skowork_group FROM role_mappings ORDER BY idp_role, skowork_group').all());
  });
  r.put('/role-mappings', requireRole('platform-admin'), (req, res) => {
    const mappings = parsePairs(req.body?.mappings, 'idp_role', 'skowork_group');
    if (!mappings) {
      sendError(res, 400, 'INVALID_REQUEST', 'mappings must be a list of {idp_role, skowork_group}.');
      return;
    }
    const before = db.prepare('SELECT idp_role, skowork_group FROM role_mappings ORDER BY idp_role, skowork_group').all();
    tx(db, () => {
      db.exec('DELETE FROM role_mappings');
      const ins = db.prepare('INSERT OR IGNORE INTO role_mappings (idp_role, skowork_group) VALUES (?, ?)');
      for (const [a, b] of mappings) ins.run(a, b);
    });
    const after = db.prepare('SELECT idp_role, skowork_group FROM role_mappings ORDER BY idp_role, skowork_group').all();
    record(req, 'identity.role-mappings.set', { before, after, reason: str(req.body?.reason) ?? null });
    hooks.onMappingChanged?.();
    res.json({ mappings: mappings.map(([idp_role, skowork_group]) => ({ idp_role, skowork_group })) });
  });

  // Direct Group membership
  r.get('/group-members', requireRole('platform-admin'), (req, res) => {
    const group = str(req.query.group);
    res.json(
      group
        ? db.prepare('SELECT user_name, skowork_group FROM group_members WHERE skowork_group = ? ORDER BY user_name').all(group)
        : db.prepare('SELECT user_name, skowork_group FROM group_members ORDER BY skowork_group, user_name').all(),
    );
  });
  r.post('/group-members', requireRole('platform-admin'), (req, res) => {
    const user = str(req.body?.user_name)?.trim();
    const group = str(req.body?.skowork_group)?.trim();
    if (!user || !group || !validName(group)) {
      sendError(res, 400, 'INVALID_REQUEST', 'user_name and skowork_group (lowercase letters, digits, - and _) are required.');
      return;
    }
    db.prepare('INSERT OR IGNORE INTO group_members (user_name, skowork_group) VALUES (?, ?)').run(user, group);
    record(req, 'identity.group-member.add', { user, group });
    hooks.onAssignmentChanged?.(user);
    res.json({ user_name: user, skowork_group: group });
  });
  r.delete('/group-members', requireRole('platform-admin'), (req, res) => {
    const user = str(req.body?.user_name ?? req.query.user_name);
    const group = str(req.body?.skowork_group ?? req.query.skowork_group);
    const r2 = db.prepare('DELETE FROM group_members WHERE user_name = ? COLLATE NOCASE AND skowork_group = ?').run(user ?? '', group ?? '');
    record(req, 'identity.group-member.remove', { user, group });
    if (r2.changes > 0 && user) hooks.onAssignmentChanged?.(user);
    res.json({ removed: r2.changes > 0 });
  });

  // Admin roles
  r.get('/admin-roles', requireRole('platform-admin'), (_req, res) => {
    res.json(db.prepare('SELECT user_name, role FROM admin_roles ORDER BY user_name, role').all());
  });
  r.post('/admin-roles', requireRole('platform-admin'), (req, res) => {
    const user = str(req.body?.user_name)?.trim();
    const role = str(req.body?.role) as AdminRole | undefined;
    if (!user || !role || !ADMIN_ROLES.includes(role)) {
      sendError(res, 400, 'INVALID_REQUEST', `user_name and role (${ADMIN_ROLES.join(', ')}) are required.`);
      return;
    }
    db.prepare('INSERT OR IGNORE INTO admin_roles (user_name, role) VALUES (?, ?)').run(user, role);
    record(req, 'identity.admin-role.grant', { user, role });
    res.json({ user_name: user, role });
  });
  r.delete('/admin-roles', requireRole('platform-admin'), (req, res) => {
    const user = str(req.body?.user_name ?? req.query.user_name);
    const role = str(req.body?.role ?? req.query.role);
    const r2 = db.prepare('DELETE FROM admin_roles WHERE user_name = ? COLLATE NOCASE AND role = ?').run(user ?? '', role ?? '');
    record(req, 'identity.admin-role.revoke', { user, role });
    res.json({ removed: r2.changes > 0 });
  });
  // Admin Directory: names the control plane has seen, for the console's pickers. Any administrator role (admin gate).
  r.get('/directory', (_req, res) => {
    const users = new Map<string, { name: string; sources: Set<string>; bootstrap: boolean }>();
    const add = (name: string | null | undefined, source: string) => {
      const n = name?.trim();
      if (!n) return;
      const u = users.get(n.toLowerCase()) ?? { name: n, sources: new Set<string>(), bootstrap: false };
      u.sources.add(source);
      users.set(n.toLowerCase(), u);
    };
    const names = (sql: string) => (db.prepare(sql).all() as { n: string }[]).map((x) => x.n);
    for (const n of names('SELECT DISTINCT user_name AS n FROM installations')) add(n, 'installation');
    for (const n of names('SELECT DISTINCT user_name AS n FROM group_members')) add(n, 'member');
    for (const n of names('SELECT DISTINCT user_name AS n FROM admin_roles')) add(n, 'administrator');
    for (const n of names("SELECT value AS n FROM deny_list WHERE kind = 'user'")) add(n, 'denied');
    for (const o of names('SELECT owners AS n FROM policy_groups')) for (const n of JSON.parse(o) as string[]) add(n, 'owner');
    for (const n of hooks.bootstrapAdmins ?? []) {
      add(n, 'administrator');
      users.get(n.toLowerCase())!.bootstrap = true;
    }
    const roles = new Set(names('SELECT DISTINCT idp_role AS n FROM role_mappings'));
    for (const j of names("SELECT DISTINCT idp_roles AS n FROM installations WHERE idp_roles != '[]'")) for (const x of JSON.parse(j) as string[]) roles.add(x);
    res.json({
      users: [...users.values()].map((u) => ({ name: u.name, sources: [...u.sources].sort(), bootstrap: u.bootstrap })).sort((a, b) => a.name.localeCompare(b.name)),
      idp_roles: [...roles].sort(),
    });
  });
  // The audit log viewer is in the admin console module (src/admin/routes.ts).
  return r;
}

export function addDenied(db: Db, kind: 'user' | 'installation', value: string, reason: string, by: string): void {
  db.prepare(
    `INSERT INTO deny_list (kind, value, reason, created_by, created_at) VALUES (?, ?, ?, ?, ?)
     ON CONFLICT (kind, value) DO UPDATE SET reason = excluded.reason`,
  ).run(kind, value, reason, by, nowIso());
}

const validName = (s: string) => /^[a-z0-9][a-z0-9_-]{0,63}$/.test(s);

function parsePairs(v: unknown, a: string, b: string): [string, string][] | null {
  if (!Array.isArray(v)) return null;
  const out: [string, string][] = [];
  for (const item of v) {
    const x = str(item?.[a])?.trim();
    const y = str(item?.[b])?.trim();
    if (!x || !y || !validName(y)) return null;
    out.push([x, y]);
  }
  return out;
}
