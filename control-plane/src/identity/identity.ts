// control-plane/identity: trust in the identity provider's access tokens, Groups, admin roles and the deny list.
import { createLocalJWKSet, createRemoteJWKSet, errors as joseErrors, jwtVerify, type JWTPayload } from 'jose';
import type { NextFunction, Request, Response } from 'express';
import type { Config } from '../config.ts';
import type { Db } from '../db.ts';
import { nowIso } from '../db.ts';
import { clientIp, sendError } from '../http.ts';
import type { Audit } from './audit.ts';

// Defined by control-plane/admin-console.
export const ADMIN_ROLES = ['platform-admin', 'security-auditor', 'group-owner', 'skill-reviewer', 'works-council-reviewer'] as const;
export type AdminRole = (typeof ADMIN_ROLES)[number];
export const EVERYONE = 'everyone';

export interface User {
  subject: string;
  userName: string;
  email: string;
  displayName: string;
  sid: string | null;
  idpRoles: string[];
  groups: string[];
  adminRoles: AdminRole[];
  tokenExpiresAt: number; // unix seconds
}

export type AuthResult =
  | { ok: true; user: User }
  | { ok: false; status: 401 | 403; code: string; reason: string; subject?: string; userName?: string };

type KeyResolver = Parameters<typeof jwtVerify>[1];
type AuthContext = { installationId?: string | null; ip?: string | null; path?: string };

/** A system registered by administrators that calls APIs with client-credentials tokens (Service Principal Tokens). */
export interface ServicePrincipal {
  client_id: string;
  name: string;
  apis: string[];
  registered_by: string;
  registered_at: string;
  retired_at: string | null;
}

export type IssuerAuthResult =
  | { ok: true; user: User; principal?: undefined }
  | { ok: true; principal: { clientId: string; name: string }; user?: undefined }
  | { ok: false; status: 401 | 403; code: string; reason: string };

/** A client-credentials token: Keycloak marks it with client_id and a service-account- user name. */
function isClientCredentials(p: JWTPayload): boolean {
  return typeof p.client_id === 'string' || (typeof p.preferred_username === 'string' && p.preferred_username.startsWith('service-account-'));
}

/** Verifies bearer tokens against the configured issuer (and, outside production, the test issuer). */
export class IdentityService {
  readonly #cfg: Config;
  readonly #db: Db;
  readonly #audit: Audit;
  #issuer: string;
  #jwks: KeyResolver | null = null;
  #discovery: Promise<void> | null = null;
  #testJwks: KeyResolver | null = null;

  constructor(cfg: Config, db: Db, audit: Audit) {
    this.#cfg = cfg;
    this.#db = db;
    this.#audit = audit;
    this.#issuer = cfg.identity.issuer;
    if (cfg.identity.testIssuer && cfg.env !== 'production') {
      this.#testJwks = createLocalJWKSet(cfg.identity.testIssuer.jwks as Parameters<typeof createLocalJWKSet>[0]);
    }
  }

  /** Loads discovery once; on failure it is retried by the next token (Production Issuer Guard: IdP down at startup). */
  async #ensureDiscovery(): Promise<void> {
    if (this.#jwks) return;
    if (!this.#discovery) {
      this.#discovery = (async () => {
        const url = this.#cfg.identity.discoveryUrl;
        if (!url) throw new Error('no identity provider configured');
        const res = await fetch(url, { signal: AbortSignal.timeout(10_000) });
        if (!res.ok) throw new Error(`discovery answered ${res.status}`);
        const doc = (await res.json()) as { issuer?: string; jwks_uri?: string };
        if (!doc.jwks_uri || !doc.issuer) throw new Error('discovery document lacks issuer or jwks_uri');
        if (!this.#issuer) this.#issuer = doc.issuer;
        // An unknown key id makes jose reload the key set, at most once per cooldown (Signing Key Refresh).
        this.#jwks = createRemoteJWKSet(new URL(doc.jwks_uri), { cooldownDuration: 60_000, timeoutDuration: 5_000 });
      })().catch((err: unknown) => {
        this.#discovery = null;
        throw err;
      });
    }
    await this.#discovery;
  }

  /** Startup probe: logs a warning when the identity provider is unreachable, never fails. */
  async warmUp(log: (msg: string) => void): Promise<void> {
    try {
      await this.#ensureDiscovery();
    } catch (err) {
      log(`warning: identity provider not reachable at startup (${(err as Error).message}); keys will be fetched on first use`);
    }
  }

  #refuser(ctx: AuthContext) {
    return (code: string, reason: string, status: 401 | 403 = 401, claims?: JWTPayload): { ok: false; status: 401 | 403; code: string; reason: string } => {
      this.#audit.write({
        category: 'security',
        action: 'identity.refused',
        outcome: 'refused',
        subject: claims?.sub ?? null,
        userName: typeof claims?.preferred_username === 'string' ? claims.preferred_username : null,
        installationId: ctx.installationId ?? null,
        sourceIp: ctx.ip ?? null,
        reason: `${code}: ${reason}`,
        details: ctx.path ? { path: ctx.path } : undefined,
      });
      return { ok: false, status, code, reason };
    };
  }

  /** Signature, issuer, audience and expiry of a token; the payload or the refusal. */
  async #verify(token: string, ctx: AuthContext): Promise<{ ok: true; payload: JWTPayload } | { ok: false; status: 401 | 403; code: string; reason: string }> {
    const refuse = this.#refuser(ctx);
    try {
      const test = this.#cfg.identity.testIssuer;
      const unverifiedIss = peekIssuer(token);
      const useTest = this.#testJwks && test && unverifiedIss === test.issuer;
      if (!useTest) await this.#ensureDiscovery();
      const result = await jwtVerify(token, useTest ? this.#testJwks! : this.#jwks!, {
        issuer: useTest ? test!.issuer : this.#issuer,
        audience: this.#cfg.identity.audiences,
        clockTolerance: this.#cfg.identity.clockSkewS,
        requiredClaims: ['sub', 'exp', 'iat'],
      });
      const payload = result.payload;
      const lifetime = (payload.exp ?? 0) - (payload.iat ?? 0);
      if (lifetime > this.#cfg.identity.maxTokenLifetimeS) return refuse('UNAUTHORIZED', `token lifetime ${lifetime}s exceeds the cap`, 401, payload);
      return { ok: true, payload };
    } catch (err) {
      if (err instanceof joseErrors.JWTExpired) return refuse('UNAUTHORIZED', 'token expired');
      if (err instanceof joseErrors.JWTClaimValidationFailed) return refuse('UNAUTHORIZED', `claim ${err.claim} invalid`);
      if (err instanceof joseErrors.JWKSNoMatchingKey) return refuse('UNAUTHORIZED', 'unknown signing key');
      if (err instanceof joseErrors.JOSEError) return refuse('UNAUTHORIZED', err.code);
      return refuse('UNAUTHORIZED', `identity provider unavailable: ${(err as Error).message}`);
    }
  }

  /**
   * Service Principal Tokens (design D8): a person's token, or a registered service principal's client-credentials
   * token for an API its registration lists.
   */
  async authenticateIssuer(token: string, ctx: AuthContext, api: string): Promise<IssuerAuthResult> {
    const refuse = this.#refuser(ctx);
    const v = await this.#verify(token, ctx);
    if (!v.ok) return v;
    const azp = typeof v.payload.azp === 'string' ? v.payload.azp : '';
    const sp = this.servicePrincipal(azp);
    if (sp) {
      if (sp.retired_at) return refuse('FORBIDDEN', `service principal ${sp.name} is retired`, 403, v.payload);
      if (!sp.apis.includes(api)) return refuse('FORBIDDEN', `service principal ${sp.name} may not call ${api}`, 403, v.payload);
      return { ok: true, principal: { clientId: sp.client_id, name: sp.name } };
    }
    if (isClientCredentials(v.payload)) return refuse('FORBIDDEN', `client ${azp || '(none)'} is not a registered service principal`, 403, v.payload);
    const r = await this.authenticate(token, ctx);
    return r.ok ? { ok: true, user: r.user } : r;
  }

  servicePrincipal(clientId: string): ServicePrincipal | undefined {
    if (!clientId) return undefined;
    const row = this.#db.prepare('SELECT client_id, name, apis, registered_by, registered_at, retired_at FROM service_principals WHERE client_id = ?').get(clientId) as
      | (Omit<ServicePrincipal, 'apis'> & { apis: string })
      | undefined;
    return row ? { ...row, apis: JSON.parse(row.apis) as string[] } : undefined;
  }

  async authenticate(token: string, ctx: AuthContext): Promise<AuthResult> {
    const refuse = this.#refuser(ctx);
    const v = await this.#verify(token, ctx);
    if (!v.ok) return v;
    const payload = v.payload;

    const azp = typeof payload.azp === 'string' ? payload.azp : '';
    if (!this.#cfg.identity.allowedClients.includes(azp)) {
      // A service principal on an API that needs a user, or any client-credentials token: 403 (design D8).
      if (this.servicePrincipal(azp)) return refuse('FORBIDDEN', `service principal ${azp} may not call this API`, 403, payload);
      if (isClientCredentials(payload)) return refuse('FORBIDDEN', `client ${azp || '(none)'} is not a registered service principal`, 403, payload);
      return refuse('UNAUTHORIZED', `client ${azp || '(none)'} not allowed`, 401, payload);
    }

    const userName = String(payload.preferred_username ?? payload.sub ?? '');
    const denied = this.findDenied(userName, ctx.installationId ?? null);
    if (denied) {
      this.#audit.write({
        category: 'security', action: 'identity.denied', outcome: 'refused', subject: payload.sub, userName,
        installationId: ctx.installationId ?? null, sourceIp: ctx.ip ?? null, reason: `${denied.kind} on deny list: ${denied.reason}`,
      });
      return { ok: false, status: 403, code: 'DENIED', reason: 'access has been revoked', subject: payload.sub, userName };
    }

    const idpRoles = Array.isArray(payload.roles) ? payload.roles.filter((r): r is string => typeof r === 'string') : [];
    const user: User = {
      subject: String(payload.sub),
      userName,
      email: typeof payload.email === 'string' ? payload.email : '',
      displayName: typeof payload.name === 'string' ? payload.name : String(payload.display_name ?? userName),
      sid: typeof payload.sid === 'string' ? payload.sid : null,
      idpRoles,
      groups: this.groupsFor(userName, idpRoles),
      adminRoles: this.adminRolesFor(userName),
      tokenExpiresAt: payload.exp ?? 0,
    };
    if (user.sid) this.#auditFirstSession(user, ctx);
    return { ok: true, user };
  }

  #auditFirstSession(user: User, ctx: { installationId?: string | null; ip?: string | null }): void {
    const r = this.#db.prepare('INSERT OR IGNORE INTO seen_sessions (sid, first_seen) VALUES (?, ?)').run(user.sid, nowIso());
    if (r.changes > 0) {
      this.#audit.write({
        category: 'identity', action: 'identity.session-first-use', outcome: 'ok', subject: user.subject, userName: user.userName,
        installationId: ctx.installationId ?? null, sourceIp: ctx.ip ?? null,
      });
    }
  }

  groupsFor(userName: string, idpRoles: string[]): string[] {
    const set = new Set<string>([EVERYONE]);
    if (idpRoles.length) {
      const placeholders = idpRoles.map(() => '?').join(',');
      for (const r of this.#db.prepare(`SELECT workbench_group FROM role_mappings WHERE idp_role IN (${placeholders})`).all(...idpRoles) as { workbench_group: string }[]) {
        set.add(r.workbench_group);
      }
    }
    for (const r of this.#db.prepare('SELECT workbench_group FROM group_members WHERE user_name = ? COLLATE NOCASE').all(userName) as { workbench_group: string }[]) {
      set.add(r.workbench_group);
    }
    return [...set].sort();
  }

  /** The identity-provider roles last seen in the user's tokens, as recorded by the fleet registry (for simulation). */
  knownRolesFor(userName: string): string[] {
    const row = this.#db
      .prepare('SELECT idp_roles FROM installations WHERE user_name = ? COLLATE NOCASE ORDER BY last_seen DESC LIMIT 1')
      .get(userName) as { idp_roles: string } | undefined;
    return row ? (JSON.parse(row.idp_roles) as string[]) : [];
  }

  adminRolesFor(userName: string): AdminRole[] {
    const set = new Set<AdminRole>();
    if (this.#cfg.identity.bootstrapAdmins.includes(userName.toLowerCase())) set.add('platform-admin');
    for (const r of this.#db.prepare('SELECT role FROM admin_roles WHERE user_name = ? COLLATE NOCASE').all(userName) as { role: AdminRole }[]) {
      set.add(r.role);
    }
    return [...set].sort();
  }

  /** Lower-case names of every user who holds one of `roles`, bootstrap admins included. */
  holdersOf(roles: readonly AdminRole[]): string[] {
    const set = new Set<string>(roles.includes('platform-admin') ? this.#cfg.identity.bootstrapAdmins : []);
    const marks = roles.map(() => '?').join(',');
    for (const r of this.#db.prepare(`SELECT user_name FROM admin_roles WHERE role IN (${marks})`).all(...roles) as { user_name: string }[]) {
      set.add(r.user_name.toLowerCase());
    }
    return [...set].sort();
  }

  findDenied(userName: string, installationId: string | null): { kind: string; reason: string } | undefined {
    return this.#db
      .prepare(
        `SELECT kind, reason FROM deny_list WHERE (kind = 'user' AND value = ? COLLATE NOCASE) OR (kind = 'installation' AND value = ?) LIMIT 1`,
      )
      .get(userName, installationId ?? '') as { kind: string; reason: string } | undefined;
  }
}

function peekIssuer(token: string): string | undefined {
  try {
    const part = token.split('.')[1];
    if (!part) return undefined;
    const iss = JSON.parse(Buffer.from(part, 'base64url').toString('utf8')).iss;
    return typeof iss === 'string' ? iss : undefined;
  } catch {
    return undefined;
  }
}

export function bearer(req: { headers: Record<string, string | string[] | undefined> }): string | null {
  const h = req.headers.authorization;
  const v = Array.isArray(h) ? h[0] : h;
  if (!v) return null;
  const m = /^Bearer\s+(.+)$/i.exec(v.trim());
  return m ? m[1]! : null;
}

declare global {
  // eslint-disable-next-line @typescript-eslint/no-namespace
  namespace Express {
    interface Request {
      user?: User;
    }
  }
}

/** Express middleware: every request needs a valid user token. */
export function requireUser(identity: IdentityService) {
  return async (req: Request, res: Response, next: NextFunction): Promise<void> => {
    const token = bearer(req);
    if (!token) {
      sendError(res, 401, 'UNAUTHORIZED', 'A bearer access token is required.');
      return;
    }
    const installationId = typeof req.body?.installation_id === 'string' ? req.body.installation_id : (req.header('x-workbench-installation') ?? null);
    const r = await identity.authenticate(token, { installationId, ip: clientIp(req), path: req.path });
    if (!r.ok) {
      res.setHeader('WWW-Authenticate', 'Bearer error="invalid_token"');
      sendError(res, r.status, r.code, r.status === 403 ? 'Access has been revoked.' : 'The access token is not valid.');
      return;
    }
    req.user = r.user;
    next();
  };
}

/** Express middleware: one of the admin roles is required (platform-admin always qualifies). */
export function requireRole(...roles: AdminRole[]) {
  return (req: Request, res: Response, next: NextFunction): void => {
    const held = req.user?.adminRoles ?? [];
    if (held.includes('platform-admin') || roles.some((r) => held.includes(r))) {
      next();
      return;
    }
    sendError(res, 403, 'FORBIDDEN', 'An administrator role is required.');
  };
}
