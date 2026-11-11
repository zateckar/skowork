// API connector endpoints (control-plane/api-connectors, design D11). The user side is behind requireUser and serves the
// Connectors list, the personal access token and the call. The administration side is behind requireRole('platform-admin').
import express, { Router, type NextFunction, type Request, type Response } from 'express';
import { bearer } from '../identity/identity.ts';
import type { ApiConnectors, Caller } from './service.ts';

const wrap =
  (fn: (req: Request) => unknown) =>
  async (req: Request, res: Response, next: NextFunction) => {
    try {
      res.json(await fn(req));
    } catch (err) {
      next(err);
    }
  };

const body = (req: Request) => (req.body && typeof req.body === 'object' && !Buffer.isBuffer(req.body) ? (req.body as Record<string, unknown>) : {});
const caller = (req: Request): Caller => ({ subject: req.user!.subject, userName: req.user!.userName, groups: req.user!.groups });
const id = (req: Request) => String(req.params.id);

/** User side, mounted at /api/v1/connectors behind requireUser. */
export function connectorRoutes(svc: ApiConnectors): Router {
  const r = Router();
  r.get('/', wrap((req) => svc.listFor(caller(req))));
  r.put('/:id/token', wrap((req) => svc.setToken(caller(req), id(req), body(req).token)));
  r.delete('/:id/token', wrap((req) => svc.removeToken(caller(req), id(req))));
  r.post('/:id/operations/:operation/call', wrap((req) => svc.call(caller(req), bearer(req), id(req), String(req.params.operation), body(req).arguments)));
  return r;
}

/** Administration side, mounted at /api/v1/admin/connectors behind requireRole('platform-admin'). */
export function connectorAdminRoutes(svc: ApiConnectors, maxDocumentBytes: number): Router {
  const r = Router();
  const actor = (req: Request): Caller => ({ subject: req.user!.subject, userName: req.user!.userName, groups: req.user!.groups });
  r.get('/', wrap(() => svc.adminList()));
  r.post('/', wrap((req) => svc.createConnector(actor(req), body(req))));
  r.get('/:id', wrap((req) => svc.adminView(id(req))));
  r.put('/:id/settings', wrap((req) => svc.saveSettings(actor(req), id(req), body(req))));
  r.put('/:id/operations', wrap((req) => svc.saveSelection(actor(req), id(req), body(req).operations)));
  r.delete('/:id', wrap((req) => svc.removeConnector(actor(req), id(req))));
  // The document is one JSON or YAML file. A JSON body is parsed by the application before this point, so both forms are accepted.
  r.put(
    '/:id/document',
    express.raw({ type: () => true, limit: maxDocumentBytes }),
    wrap((req) => {
      const b = req.body as unknown;
      const text = Buffer.isBuffer(b) ? b.toString('utf8') : typeof b === 'string' ? b : JSON.stringify(b ?? {});
      return svc.uploadDocument(actor(req), id(req), text);
    }),
  );
  return r;
}
