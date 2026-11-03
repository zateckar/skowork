// Policy endpoints: the client bundle, the public verification keys, and the administration API
// (control-plane/policy-service Administration API, Policy Retrieval By Clients; design D14).
import { Router, type Request } from 'express';
import type { Audit } from '../identity/audit.ts';
import type { IdentityService } from '../identity/identity.ts';
import { ApiError, str } from '../http.ts';
import { GUARDRAIL_SCOPE, type PolicyService, groupScope } from './service.ts';

/** Client side: GET /api/v1/policy/bundle (behind requireUser) and the public key set. */
export function policyRoutes(svc: PolicyService): Router {
  const r = Router();
  r.get('/.well-known/policy-keys.json', async (_req, res, next) => {
    try {
      res.setHeader('Cache-Control', 'public, max-age=300');
      res.json(await svc.publicKeys());
    } catch (err) {
      next(err);
    }
  });
  r.get('/api/v1/policy/bundle', async (req, res, next) => {
    try {
      const u = req.user!;
      const b = await svc.bundleFor(u);
      res.json({ bundle: b.bundle, version: b.version, expires_at: b.expires_at, stale: b.stale });
    } catch (err) {
      next(err);
    }
  });
  return r;
}

const actor = (req: Request) => req.user!;
const reason = (req: Request) => str(req.body?.reason)?.trim() ?? '';

/** Admin side, mounted at /api/v1/admin/policy (behind requireUser and an admin role). */
export function policyAdminRoutes(svc: PolicyService, identity: IdentityService, audit: Audit): Router {
  const r = Router();
  const wrap =
    (fn: (req: Request) => unknown) =>
    async (req: Request, res: import('express').Response, next: import('express').NextFunction) => {
      try {
        res.json(await fn(req));
      } catch (err) {
        next(err);
      }
    };

  const platformOrAuditor = (req: Request) => {
    const roles = actor(req).adminRoles;
    if (!roles.includes('platform-admin') && !roles.includes('security-auditor')) throw new ApiError(403, 'FORBIDDEN', 'An administrator role is required.');
  };
  const groupsOf = (userName: string) => {
    // A user's Groups: the direct assignments and the roles last seen on their installations (control-plane/identity).
    return identity.groupsFor(userName, identity.knownRolesFor(userName));
  };

  r.get('/schema', wrap((req) => ({
    version: svc.registry.version,
    schema: svc.registry.jsonSchema(),
    guardrail_schema: svc.registry.jsonSchema({ guardrail: true }),
    ...(req.query.keys === 'true' && { keys: [...svc.registry.keys.values()] }),
  })));

  r.get('/groups', wrap(() => svc.listGroups()));
  r.put('/groups/:name', wrap((req) => {
    const owners = Array.isArray(req.body?.owners) ? (req.body.owners as unknown[]).filter((o): o is string => typeof o === 'string' && o.trim() !== '').map((o) => o.trim()) : [];
    return svc.defineGroup(actor(req), String(req.params.name), str(req.body?.description) ?? '', owners);
  }));

  // The same version, draft and rollout endpoints for a Group and for the org guardrail.
  const scopeRoutes = (prefix: string, scopeOf: (req: Request) => string) => {
    const readable = (req: Request) => {
      const scope = scopeOf(req);
      if (!svc.canManage(actor(req), scope)) platformOrAuditor(req);
      return scope;
    };
    r.get(`${prefix}/versions`, wrap((req) => svc.versions(readable(req))));
    r.get(`${prefix}/adoption`, wrap((req) => svc.adoption(readable(req))));
    r.get(`${prefix}/versions/:ref`, wrap((req) => svc.version(readable(req), String(req.params.ref))));
    r.put(`${prefix}/draft`, wrap((req) => {
      const priority = req.body?.priority === undefined ? 0 : Number(req.body.priority);
      return svc.saveDraft(actor(req), scopeOf(req), req.body?.document, priority, reason(req));
    }));
    r.delete(`${prefix}/draft`, wrap((req) => svc.deleteDraft(actor(req), scopeOf(req))));
    r.post(`${prefix}/publish`, wrap((req) => {
      const ro = req.body?.rollout;
      const rollout = ro && typeof ro === 'object' ? { pilot_group: ro.pilot_group, percent: ro.percent } : undefined;
      return svc.publish(actor(req), scopeOf(req), { reason: reason(req), rollout });
    }));
    r.post(`${prefix}/rollout/promote`, wrap((req) => svc.promote(actor(req), scopeOf(req), reason(req))));
    r.post(`${prefix}/rollout/cancel`, wrap((req) => svc.cancelRollout(actor(req), scopeOf(req), reason(req))));
    r.post(`${prefix}/rollback`, wrap((req) => svc.rollback(actor(req), scopeOf(req), reason(req))));
    r.get(`${prefix}/diff`, wrap((req) => {
      const scope = readable(req);
      const user = str(req.query.user);
      return svc.diff(scope, str(req.query.from) ?? 'current', str(req.query.to) ?? 'draft', user ? { userName: user, groups: groupsOf(user) } : undefined);
    }));
    r.get(`${prefix}/audit`, wrap((req) => {
      const scope = readable(req);
      return audit.query({ category: 'policy', limit: Number(req.query.limit) || 100 }).filter((e) => (e.details as { scope?: string } | null)?.scope === scope);
    }));
  };
  scopeRoutes('/groups/:name', (req) => groupScope(String(req.params.name)));
  scopeRoutes('/guardrail', () => GUARDRAIL_SCOPE);

  r.post('/simulate', wrap((req) => {
    const groups = Array.isArray(req.body?.groups) ? (req.body.groups as unknown[]).filter((g): g is string => typeof g === 'string') : undefined;
    const user = str(req.body?.user);
    const a = actor(req);
    // A group-owner may simulate only sets made of Groups they own; other roles see everything.
    if (!a.adminRoles.includes('platform-admin') && !a.adminRoles.includes('security-auditor')) {
      const names = user ? groupsOf(user) : (groups ?? []);
      if (user || !names.every((g) => svc.canManage(a, groupScope(g)))) throw new ApiError(403, 'FORBIDDEN', 'You may simulate only the Groups you own.');
    }
    return svc.simulate({ user, groups, drafts: req.body?.drafts === true }, groupsOf);
  }));

  r.get('/dispatch-rules', wrap((req) => (platformOrAuditor(req), svc.listDispatchRules())));
  r.post('/dispatch-rules', wrap((req) => svc.addDispatchRule(actor(req), {
    issuer_kind: req.body?.issuer_kind, issuer: str(req.body?.issuer) ?? '', target_kind: req.body?.target_kind, target: str(req.body?.target) ?? '',
  })));
  r.delete('/dispatch-rules/:id', wrap((req) => svc.removeDispatchRule(actor(req), Number(req.params.id))));

  r.post('/keys/rotate', wrap((req) => svc.rotateKeys(actor(req))));
  return r;
}
