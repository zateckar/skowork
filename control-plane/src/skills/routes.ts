// Skill registry endpoints: the client API under /api/v1/skills (behind requireUser), the public signing keys, and the
// review API under /api/v1/admin/skills (behind requireView('skills')).
import express, { Router, type NextFunction, type Request, type Response } from 'express';
import { sendError, str } from '../http.ts';
import { requireRole } from '../identity/identity.ts';
import type { SkillRegistry } from './registry.ts';

const wrap =
  (fn: (req: Request) => unknown) =>
  async (req: Request, res: Response, next: NextFunction) => {
    try {
      res.json(await fn(req));
    } catch (err) {
      next(err);
    }
  };

const names = (v: unknown): string[] => (str(v) ?? '').split(',').map((s) => s.trim()).filter(Boolean);
const raw = (limit: number) => express.raw({ type: () => true, limit });
const bytesOf = (req: Request) => (Buffer.isBuffer(req.body) ? req.body : Buffer.alloc(0));
const body = (req: Request) => (req.body && typeof req.body === 'object' && !Buffer.isBuffer(req.body) ? (req.body as Record<string, unknown>) : {});

export function skillRoutes(reg: SkillRegistry, maxPackageBytes: number): Router {
  const r = Router();
  r.get('/.well-known/workbench-skill-keys.json', async (_req, res, next) => {
    try {
      res.setHeader('Cache-Control', 'public, max-age=300');
      res.json(await reg.keys.publicKeys());
    } catch (err) {
      next(err);
    }
  });
  // Publish: the zip is the body; scope and audience are query parameters (?scope=group&groups=a,b or &users=x,y).
  r.post(
    '/api/v1/skills/packages',
    raw(maxPackageBytes),
    wrap((req) => reg.publish(req.user!, bytesOf(req), { scope: str(req.query.scope), users: names(req.query.users), groups: names(req.query.groups) })),
  );
  r.get('/api/v1/skills/sync', wrap((req) => reg.syncFor(req.user!)));
  r.get('/api/v1/skills/content/:digest', (req, res) => {
    const bytes = reg.content(req.user!, String(req.params.digest));
    if (!bytes) return sendError(res, 404, 'NOT_FOUND', 'No such Skill content.');
    res.setHeader('content-type', 'application/zip');
    res.setHeader('cache-control', 'private, max-age=31536000, immutable');
    res.end(bytes);
  });
  r.get('/api/v1/skills/mine', wrap((req) => reg.mine(req.user!)));
  r.post('/api/v1/skills/usage', wrap((req) => reg.reportUsage(req.body)));
  r.get('/api/v1/skills/:name/usage', wrap((req) => reg.usage(req.user!, String(req.params.name))));
  r.post('/api/v1/skills/:name/versions/:version/withdraw', wrap((req) => reg.withdraw(req.user!, String(req.params.name), String(req.params.version))));
  r.post('/api/v1/skills/:name/revoke', wrap((req) => reg.revoke(req.user!, String(req.params.name), str(body(req).version), body(req).reason)));
  return r;
}

/** Mounted at /api/v1/admin/skills behind requireView('skills') (skill-reviewer, platform-admin). */
export function skillAdminRoutes(reg: SkillRegistry, maxPackageBytes: number): Router {
  const r = Router();
  r.get('/queue', wrap(() => reg.queue()));
  r.get('/', wrap(() => reg.all()));
  r.get('/versions/:id', wrap((req) => ({ ...reg.detail(Number(req.params.id)), approval: reg.approvalFor(req.user!, Number(req.params.id)) })));
  r.get('/versions/:id/content', (req, res, next) => {
    try {
      const d = reg.detail(Number(req.params.id));
      const bytes = reg.content(req.user!, d.digest);
      if (!bytes) return sendError(res, 404, 'NOT_FOUND', 'No such Skill content.');
      res.setHeader('content-type', 'application/zip');
      res.setHeader('content-disposition', `attachment; filename="${d.name}-${d.version}.zip"`);
      res.end(bytes);
    } catch (err) {
      next(err);
    }
  });
  r.post('/versions/:id/approve', wrap((req) => reg.approve(req.user!, Number(req.params.id), body(req).comment)));
  r.post('/versions/:id/reject', wrap((req) => reg.reject(req.user!, Number(req.params.id), body(req).comment)));
  r.post('/:name/revoke', wrap((req) => reg.revoke(req.user!, String(req.params.name), str(body(req).version), body(req).reason)));
  // Public import (platform-admin only): ?url=... to download, or the zip as the body with an optional ?source_url=...;
  // &licence=... unless SKILL.md declares license.
  r.post(
    '/import',
    requireRole('platform-admin'),
    raw(maxPackageBytes),
    wrap((req) => reg.import(req.user!, bytesOf(req), { url: str(req.query.url), source_url: str(req.query.source_url), licence: str(req.query.licence) })),
  );
  r.post('/keys/rotate', requireRole('platform-admin'), wrap(() => reg.keys.rotate()));
  return r;
}
