// control-plane/task-dispatch HTTP API: the issuer API (people and service principals) and the platform-admin settings.
import { Router, type NextFunction, type Request, type Response } from 'express';
import { clientIp, sendError } from '../http.ts';
import { bearer, type IdentityService } from '../identity/identity.ts';
import { serviceIssuer, userIssuer, type TaskDispatch, type TaskIssuer } from './dispatch.ts';

declare global {
  // eslint-disable-next-line @typescript-eslint/no-namespace
  namespace Express {
    interface Request {
      issuer?: TaskIssuer;
    }
  }
}

const wrap = (fn: (req: Request, res: Response) => unknown) => async (req: Request, res: Response, next: NextFunction) => {
  try {
    res.json(await fn(req, res));
  } catch (err) {
    next(err);
  }
};
const body = (req: Request) => (req.body ?? {}) as Record<string, unknown>;

/** Issuer Authentication: a person's access token, or a service principal's token registered for this API. */
export function requireIssuer(identity: IdentityService, api: string) {
  return async (req: Request, res: Response, next: NextFunction): Promise<void> => {
    const token = bearer(req);
    if (!token) return sendError(res, 401, 'UNAUTHORIZED', 'A bearer access token is required.');
    const r = await identity.authenticateIssuer(token, { ip: clientIp(req), path: req.path }, api);
    if (!r.ok) {
      res.setHeader('WWW-Authenticate', 'Bearer error="invalid_token"');
      return sendError(res, r.status, r.code, r.status === 403 ? 'This client may not call this API.' : 'The access token is not valid.');
    }
    req.issuer = r.user ? userIssuer(r.user) : serviceIssuer(r.principal!);
    if (r.user) req.user = r.user;
    next();
  };
}

export function taskRoutes(tasks: TaskDispatch): Router {
  const r = Router();
  r.post('/', async (req, res, next) => {
    try {
      res.status(201).json(tasks.create(req.issuer!, req.body, clientIp(req)));
    } catch (err) {
      next(err);
    }
  });
  r.get('/:id', wrap((req) => tasks.status(req.issuer!, String(req.params.id))));
  r.post('/:id/cancel', wrap((req) => tasks.cancel(req.issuer!, String(req.params.id), clientIp(req))));
  return r;
}

/** Remote Task Administration: per-issuer rate limits and the callback allowlist (platform-admin). */
export function taskAdminRoutes(tasks: TaskDispatch): Router {
  const r = Router();
  r.get('/limits', wrap(() => tasks.listLimits()));
  r.put('/limits', wrap((req) => tasks.setLimit(req.user!, String(body(req).issuer ?? ''), Number(body(req).per_hour))));
  r.delete('/limits/:issuer', wrap((req) => tasks.removeLimit(req.user!, String(req.params.issuer))));
  r.get('/callbacks', wrap(() => tasks.listCallbacks()));
  r.post('/callbacks', wrap((req) => tasks.addCallback(req.user!, String(body(req).prefix ?? ''))));
  r.post('/callbacks/remove', wrap((req) => tasks.removeCallback(req.user!, String(body(req).prefix ?? ''))));
  return r;
}

/** Identity Administration: service principals and the APIs each may call (platform-admin). */
export function servicePrincipalRoutes(tasks: TaskDispatch): Router {
  const r = Router();
  r.get('/', wrap(() => tasks.listPrincipals()));
  r.post('/', wrap((req) => tasks.registerPrincipal(req.user!, req.body)));
  r.post('/:clientId/retire', wrap((req) => tasks.retirePrincipal(req.user!, String(req.params.clientId))));
  return r;
}
