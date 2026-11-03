// Admin API for LLM connections and their models (add-llm-connections task 1.3); platform-admins only.
import { Router, type NextFunction, type Request, type Response } from 'express';
import type { Connections } from './connections.ts';

const wrap =
  (fn: (req: Request) => unknown) =>
  async (req: Request, res: Response, next: NextFunction) => {
    try {
      res.json(await fn(req));
    } catch (err) {
      next(err);
    }
  };

export function llmAdminRoutes(c: Connections): Router {
  const r = Router();
  const body = (req: Request) => (req.body && typeof req.body === 'object' ? (req.body as Record<string, unknown>) : {});
  const id = (req: Request) => Number(req.params.id);
  r.get('/connections', wrap(() => c.list()));
  r.post('/connections', wrap((req) => c.create(req.user!, body(req))));
  r.get('/connections/:id', wrap((req) => c.get(id(req))));
  r.patch('/connections/:id', wrap((req) => c.update(req.user!, id(req), body(req))));
  r.get('/connections/:id/uses', wrap((req) => ({ uses: c.usesOf(id(req)) })));
  r.post('/connections/:id/test', wrap((req) => c.test(req.user!, id(req))));
  r.delete('/connections/:id', wrap((req) => c.remove(req.user!, id(req), req.query.confirm === 'true')));
  r.put('/connections/:id/models/:model', wrap((req) => c.setModel(req.user!, id(req), String(req.params.model), body(req))));
  r.get('/models', wrap(() => c.catalog()));
  return r;
}
