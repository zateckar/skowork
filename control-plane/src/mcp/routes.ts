// MCP catalog endpoints: the package download for clients (behind requireUser) and the administration API
// (control-plane/mcp-catalog Catalog Administration, platform-admin only).
import express, { Router, type NextFunction, type Request, type Response } from 'express';
import { sendError, str } from '../http.ts';
import type { McpCatalog } from './catalog.ts';

const wrap =
  (fn: (req: Request) => unknown) =>
  async (req: Request, res: Response, next: NextFunction) => {
    try {
      res.json(await fn(req));
    } catch (err) {
      next(err);
    }
  };

/** Client side: GET /api/v1/mcp/packages/{sha256}. */
export function mcpRoutes(catalog: McpCatalog): Router {
  const r = Router();
  r.get('/api/v1/mcp/packages/:sha256', (req, res) => {
    const bytes = catalog.packageFor(req.user!.groups, String(req.params.sha256));
    // A package outside the user's Groups is answered as if it did not exist.
    if (!bytes) return sendError(res, 404, 'NOT_FOUND', 'No such package.');
    res.setHeader('content-type', 'application/octet-stream');
    res.setHeader('cache-control', 'private, max-age=31536000, immutable');
    res.end(bytes);
  });
  return r;
}

/** Admin side, mounted at /api/v1/admin/mcp behind requireRole('platform-admin'). */
export function mcpAdminRoutes(catalog: McpCatalog, maxPackageBytes: number): Router {
  const r = Router();
  const body = (req: Request) => (req.body && typeof req.body === 'object' && !Buffer.isBuffer(req.body) ? (req.body as Record<string, unknown>) : {});
  r.get('/servers', wrap(() => catalog.list()));
  r.post('/servers', wrap((req) => catalog.create(req.user!, body(req))));
  r.get('/servers/:id', wrap((req) => catalog.get(String(req.params.id))));
  r.put('/servers/:id', wrap((req) => catalog.update(req.user!, String(req.params.id), body(req))));
  r.delete('/servers/:id', wrap((req) => catalog.remove(req.user!, String(req.params.id))));
  r.get('/servers/:id/versions', wrap((req) => catalog.versions(String(req.params.id))));
  r.post('/servers/:id/discover', wrap((req) => catalog.rediscover(req.user!, String(req.params.id))));
  r.put('/servers/:id/tools/:tool', wrap((req) => catalog.decideTool(req.user!, String(req.params.id), String(req.params.tool), body(req).decision)));
  r.get('/packages', wrap(() => catalog.listPackages()));
  r.put(
    '/packages',
    express.raw({ type: () => true, limit: maxPackageBytes }),
    wrap((req) => catalog.uploadPackage(req.user!, Buffer.isBuffer(req.body) ? req.body : Buffer.alloc(0), str(req.query.name) ?? '')),
  );
  return r;
}
