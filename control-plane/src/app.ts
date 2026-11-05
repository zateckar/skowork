// Wires the modules into one Express app and one HTTP server (design D10: one process, one container).
import { createServer, type Server } from 'node:http';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import express, { type NextFunction, type Request, type Response } from 'express';
import type { Config } from './config.ts';
import { openDb, type Db } from './db.ts';
import { ApiError, sendError } from './http.ts';
import { Audit } from './identity/audit.ts';
import { IdentityService, requireRole, requireUser } from './identity/identity.ts';
import { identityAdminRoutes, identityRoutes } from './identity/routes.ts';
import { FleetRegistry, fleetAdminRoutes, fleetRoutes } from './fleet/fleet.ts';
import { PushHub } from './fleet/push.ts';
import { Gateway, gatewayRoutes } from './gateway/gateway.ts';
import { llmAdminRoutes } from './gateway/admin.ts';
import { Messaging, messagingRoutes } from './messaging/messaging.ts';
import { TeamKnowledge, teamKnowledgeAdminRoutes, teamKnowledgeRoutes } from './knowledge/team.ts';
import { PolicyService } from './policy/service.ts';
import { policyAdminRoutes, policyRoutes } from './policy/routes.ts';
import { McpCatalog } from './mcp/catalog.ts';
import { mcpAdminRoutes, mcpRoutes } from './mcp/routes.ts';
import { TelemetryIngest, type TelemetryLevel } from './telemetry/ingest.ts';
import { telemetryAdminRoutes, telemetryRoutes } from './telemetry/routes.ts';
import { adminGate, requireView } from './admin/authz.ts';
import { adminConsoleRoutes } from './admin/routes.ts';
import { SkillRegistry } from './skills/registry.ts';
import { skillAdminRoutes, skillRoutes } from './skills/routes.ts';
import { TaskDispatch } from './tasks/dispatch.ts';
import { requireIssuer, servicePrincipalRoutes, taskAdminRoutes, taskRoutes } from './tasks/routes.ts';

export interface ControlPlane {
  server: Server;
  db: Db;
  identity: IdentityService;
  push: PushHub;
  fleet: FleetRegistry;
  gateway: Gateway;
  policy: PolicyService;
  catalog: McpCatalog;
  telemetry: TelemetryIngest;
  skills: SkillRegistry;
  tasks: TaskDispatch;
  audit: Audit;
  team: TeamKnowledge;
  messaging: Messaging;
  /** Whether the admin console build was found and is served at /admin/. */
  consoleBuilt: boolean;
  listen(port?: number, host?: string): Promise<number>;
  close(): Promise<void>;
}

export function createControlPlane(
  cfg: Config,
  log: (m: string) => void = console.log,
  opts: { adminDist?: string } = {},
): ControlPlane {
  const db = openDb(cfg.dbPath);
  const audit = new Audit(db);
  const identity = new IdentityService(cfg, db, audit);
  const push = new PushHub(cfg, db, audit, identity);
  const fleet = new FleetRegistry(cfg, db, audit, push);
  const gateway = new Gateway(cfg, db, audit, log);
  const policy = new PolicyService(cfg, db, audit, push);
  gateway.setPolicySource((u) => policy.compileFor(u.groups, u.userName).flat);
  const team = new TeamKnowledge(cfg, db, audit, (u, input) => gateway.embedFor(u, input), (u) => {
    const v = policy.compileFor(u.groups, u.userName).flat.get('knowledge.sharing.groups');
    return Array.isArray(v) ? v.map(String) : [];
  });
  const messaging = new Messaging(cfg, db, audit, push, (u) => (policy.compileFor(u.groups, u.userName).flat.get('messaging.mode') === 'off' ? 'off' : 'on'));
  gateway.connections.setUseSource(() => [
    ...policy.modelUses(),
    ...Object.entries(cfg.gateway.models).map(([role, model]) => ({ scope: 'server default', role, model })),
  ]);
  const catalog = new McpCatalog(cfg, db, audit, { onChanged: (groups) => policy.catalogChanged(groups) });
  policy.setCatalogSource(catalog);
  const telemetry = new TelemetryIngest(cfg.telemetry, db, audit, (u) => {
    const level = policy.compileFor(u.groups, u.userName).flat.get('telemetry.level');
    return level === 'off' || level === 'content' ? (level as TelemetryLevel) : 'metadata';
  });
  const skills = new SkillRegistry(cfg.skills, db, audit, push, (u) => {
    const scopes = policy.compileFor(u.groups, u.userName).flat.get('skills.publish');
    return Array.isArray(scopes) ? (scopes as string[]) : ['shared'];
  });
  const tasks = new TaskDispatch(db, audit, identity, policy, push);
  push.taskSink = (m) => tasks.handleClient(m);

  const app = express();
  app.disable('x-powered-by');
  app.set('trust proxy', false);
  app.use(express.json({ limit: cfg.gateway.maxRequestBytes }));

  app.get('/healthz', (_req, res) => {
    res.json({ status: 'ok' });
  });
  app.get('/readyz', (_req, res) => {
    db.prepare('SELECT 1').get();
    res.json({ status: 'ready', connected_clients: push.connectedCount });
  });

  // Public sign-in settings for the admin console (no secrets: the client is public and uses PKCE).
  app.get('/api/v1/public/admin-config', (_req, res) => {
    res.json({
      discovery_url: cfg.identity.discoveryUrl,
      client_id: cfg.identity.allowedClients[0] ?? '',
      redirect_uri: process.env.WORKBENCH_ADMIN_REDIRECT_URI ?? process.env.OIDC_REDIRECT_URI ?? '',
      scope: process.env.OIDC_SCOPE ?? 'openid profile email',
    });
  });

  // Admin console (React build), public static files; the API below needs tokens.
  // The root redirects to the console (Console Entry Point).
  const adminDist = opts.adminDist ?? fileURLToPath(new URL('../dist/admin', import.meta.url));
  const consoleBuilt = existsSync(adminDist);
  if (consoleBuilt) {
    // The query is kept: with the redirect URI at the root, the sign-in response (?code=…&state=…) arrives here.
    app.get('/', (req, res) => {
      const q = req.originalUrl.indexOf('?');
      res.redirect(302, '/admin/' + (q >= 0 ? req.originalUrl.slice(q) : ''));
    });
    app.use('/admin', express.static(adminDist, { index: 'index.html', maxAge: '1h' }));
    app.get(/^\/admin(\/.*)?$/, (_req, res) => res.sendFile('index.html', { root: adminDist }));
  }

  const user = requireUser(identity);
  app.use('/api/v1/identity', user);
  app.use('/api/v1/fleet', user);
  app.use('/api/v1/policy', user);
  app.use('/api/v1/admin', user);
  app.use('/api/v1/mcp', user);
  app.use('/api/v1/telemetry', user);
  app.use('/api/v1/skills', user);
  app.use('/api/v1/knowledge', user, teamKnowledgeRoutes(team));
  app.use('/api/v1/messaging', user, messagingRoutes(messaging));
  app.use('/llm/v1', user);

  app.use(identityRoutes());
  app.use(mcpRoutes(catalog));
  app.use(policyRoutes(policy));
  app.use(fleetRoutes(cfg, fleet, (u, inst) => Boolean(identity.findDenied(u.userName, inst))));
  app.use(gatewayRoutes(gateway));
  app.use(telemetryRoutes(cfg.telemetry, telemetry));
  app.use(skillRoutes(skills, cfg.skills.maxPackageBytes));
  // Task dispatch: people and registered service principals (Issuer Authentication).
  app.use('/api/v1/tasks', requireIssuer(identity, 'tasks'), taskRoutes(tasks));
  // Admin API: no admin role, no access; every refusal is audited (design D6).
  app.use('/api/v1/admin', adminGate(audit));
  app.use('/api/v1/admin', adminConsoleRoutes(db, audit, telemetry, (group) => {
    const flat = policy.compileFor([...new Set(['everyone', group])], null).flat;
    const tokens = flat.get('llm.budget.tokens');
    const period = flat.get('llm.budget.period');
    return { tokens: typeof tokens === 'number' ? tokens : null, period: typeof period === 'string' ? period : null };
  }));
  app.use('/api/v1/admin/identity/service-principals', requireRole('platform-admin'), servicePrincipalRoutes(tasks));
  app.use('/api/v1/admin/tasks', requireRole('platform-admin'), taskAdminRoutes(tasks));
  app.use('/api/v1/admin/identity', identityAdminRoutes(db, audit, {
    onDenied: (kind, value) => fleet.revoke(kind, value),
    onMappingChanged: () => policy.mappingChanged(),
    onAssignmentChanged: (u) => policy.assignmentChanged(u),
  }));
  app.use('/api/v1/admin/fleet', fleetAdminRoutes(db, audit, fleet, push));
  app.use('/api/v1/admin/policy', requireView('policy'), policyAdminRoutes(policy, identity, audit));
  app.use('/api/v1/admin/telemetry', telemetryAdminRoutes(telemetry));
  app.use('/api/v1/admin/llm', requireRole('platform-admin'), llmAdminRoutes(gateway.connections));
  app.use('/api/v1/admin/knowledge', requireRole('platform-admin'), teamKnowledgeAdminRoutes(team));
  app.get('/api/v1/admin/messaging/counts', requireView('dashboards'), (_req, res) => void res.json(messaging.counts()));
  app.use('/api/v1/admin/mcp', requireRole('platform-admin'), mcpAdminRoutes(catalog, cfg.mcpCatalog.maxPackageBytes));
  app.use('/api/v1/admin/skills', requireView('skills'), skillAdminRoutes(skills, cfg.skills.maxPackageBytes));

  app.use((_req, res) => sendError(res, 404, 'NOT_FOUND', 'No such endpoint.'));
  app.use((err: unknown, req: Request, res: Response, _next: NextFunction) => {
    if (err instanceof ApiError) return sendError(res, err.status, err.code, err.message, err.extra);
    const e = err as { type?: string; status?: number };
    if (e?.type === 'entity.too.large' && req.path.startsWith('/api/v1/admin/mcp/packages')) {
      return sendError(res, 413, 'TOO_LARGE', `The package is larger than the limit of ${cfg.mcpCatalog.maxPackageBytes} bytes.`);
    }
    if (e?.type === 'entity.too.large' && /^\/api\/v1\/(admin\/)?skills\//.test(req.path)) {
      return sendError(res, 413, 'TOO_LARGE', `The package is larger than the limit of ${cfg.skills.maxPackageBytes} bytes.`);
    }
    if (e?.type === 'entity.too.large') return sendError(res, 400, 'CONTEXT_LENGTH', 'The request is larger than the maximum request size.', { reason: 'request size' });
    if (e?.type === 'entity.parse.failed') return sendError(res, 400, 'INVALID_REQUEST', 'The request body is not valid JSON.');
    log(`unhandled error: ${(err as Error)?.stack ?? String(err)}`);
    if (!res.headersSent) sendError(res, 500, 'INTERNAL', 'The request could not be processed.');
  });

  // One server per listen address (HOST may list several, for example "127.0.0.1,::1").
  const servers: Server[] = [];
  const makeServer = () => {
    const s = createServer(app);
    s.on('upgrade', (req, socket, head) => {
      const path = new URL(req.url ?? '/', 'http://localhost').pathname;
      if (path === '/api/v1/fleet/push') void push.handleUpgrade(req, socket, head);
      else socket.destroy();
    });
    servers.push(s);
    return s;
  };
  const server = makeServer();

  const maintenance = setInterval(() => {
    push.prune();
    fleet.prune();
    telemetry.prune();
  }, 3600_000);
  maintenance.unref();
  const forwarding = setInterval(() => void telemetry.forwardOnce(), cfg.telemetry.forwardIntervalMs);
  forwarding.unref();
  void identity.warmUp(log);

  return {
    server, db, identity, push, fleet, gateway, policy, catalog, telemetry, skills, tasks, audit, team, messaging, consoleBuilt,
    async listen(port = cfg.port, host = cfg.host) {
      const hosts = host.split(',').map((h) => h.trim()).filter(Boolean);
      let bound = port;
      for (const [i, h] of hosts.entries()) {
        const s = i === 0 ? server : makeServer();
        bound = await new Promise<number>((resolve, reject) => {
          s.once('error', reject);
          s.listen(bound, h, () => {
            const addr = s.address();
            resolve(typeof addr === 'object' && addr ? addr.port : bound);
          });
        });
      }
      return bound;
    },
    async close() {
      clearInterval(maintenance);
      clearInterval(forwarding);
      tasks.close();
      push.close();
      gateway.close();
      messaging.close();
      for (const s of servers) {
        s.closeAllConnections();
        await new Promise<void>((r) => s.close(() => r()));
      }
      db.close();
    },
  };
}
