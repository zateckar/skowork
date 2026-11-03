// control-plane/identity Audience-Scoped Token Exchange and Exchanged Token Signing Key Publication
// (add-core-tools-and-mcp design D6). Exchanged tokens are signed with their own key ring, separate from policy keys.
import { randomUUID } from 'node:crypto';
import { SignJWT, type JWK } from 'jose';
import type { Config } from '../config.ts';
import type { Db } from '../db.ts';
import { ApiError } from '../http.ts';
import { KeyRing } from '../keys.ts';
import type { Audit } from './audit.ts';
import type { User } from './identity.ts';

export const DISCOVERY_SUBJECT = 'workbench-catalog-discovery';
const DISCOVERY_LIFETIME_S = 60;

export class TokenExchange {
  readonly #cfg: Config;
  readonly #audit: Audit;
  readonly #keys: KeyRing;
  #audiences: (groups: string[]) => string[] = () => [];

  constructor(cfg: Config, db: Db, audit: Audit) {
    this.#cfg = cfg;
    this.#audit = audit;
    // An exchanged token lives no longer than an accepted user token, so a retired key is kept for that plus a margin.
    this.#keys = new KeyRing(db, 'mcp-token', () => cfg.identity.maxTokenLifetimeS + cfg.identity.clockSkewS + 3600);
  }

  /** The issuer claim of exchanged tokens; MCP servers trust it together with the published keys. */
  get issuer(): string {
    return this.#cfg.publicBaseUrl || 'urn:workbench:control-plane';
  }

  /** control-plane/mcp-catalog Remote Audience Authorization. */
  setAudienceSource(fn: (groups: string[]) => string[]): void {
    this.#audiences = fn;
  }

  allowedAudiences(groups: string[]): string[] {
    return [...new Set([...this.#cfg.identity.mcpAudiences, ...this.#audiences(groups)])];
  }

  async #sign(claims: Record<string, unknown>, subject: string, audience: string, exp: number): Promise<string> {
    const key = await this.#keys.current();
    return new SignJWT({ ...claims, jti: randomUUID() })
      .setProtectedHeader({ alg: 'ES256', kid: key.kid, typ: 'JWT' })
      .setIssuer(this.issuer)
      .setSubject(subject)
      .setAudience(audience)
      .setIssuedAt()
      .setExpirationTime(exp)
      .sign(key.key);
  }

  async exchange(user: User, audience: string, ip: string | null): Promise<{ access_token: string; token_type: 'Bearer'; audience: string; expires_at: number }> {
    const refuse = (reason: string) => {
      this.#audit.write({
        category: 'security', action: 'identity.token-exchange', outcome: 'refused', subject: user.subject, userName: user.userName,
        sourceIp: ip, reason, details: { audience },
      });
      return new ApiError(403, 'FORBIDDEN', 'You may not obtain a token for this audience.', { reason });
    };
    if (!audience) throw new ApiError(400, 'INVALID_REQUEST', 'audience is required.');
    if (!this.allowedAudiences(user.groups).includes(audience)) throw refuse('the audience is not assigned to your Groups');
    const exp = user.tokenExpiresAt;
    if (exp <= Math.floor(Date.now() / 1000)) throw refuse('the presented token has expired');
    const token = await this.#sign({ preferred_username: user.userName, azp: 'workbench' }, user.subject, audience, exp);
    this.#audit.write({
      category: 'identity', action: 'identity.token-exchange', outcome: 'ok', subject: user.subject, userName: user.userName, sourceIp: ip,
      details: { audience, expires_at: exp },
    });
    return { access_token: token, token_type: 'Bearer', audience, expires_at: exp };
  }

  /** A short-lived token the catalog presents to a remote server during tool discovery. */
  discoveryToken(audience: string): Promise<string> {
    return this.#sign({ azp: 'workbench' }, DISCOVERY_SUBJECT, audience, Math.floor(Date.now() / 1000) + DISCOVERY_LIFETIME_S);
  }

  publicKeys(): Promise<{ keys: JWK[] }> {
    return this.#keys.publicKeys();
  }

  async rotate(actor: Pick<User, 'subject' | 'userName'>) {
    const r = await this.#keys.rotate();
    this.#audit.write({ category: 'admin', action: 'identity.token-keys.rotate', outcome: 'ok', subject: actor.subject, userName: actor.userName, details: r });
    return r;
  }
}
