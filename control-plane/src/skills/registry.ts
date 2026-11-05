// control-plane/skill-registry: storage and immutable versions, scopes and audiences, publish rights, the scan, org
// review and signing, public import, distribution, pushes, withdrawal, revocation and usage counts (add-skills D6).
import { createHash } from 'node:crypto';
import { SignJWT } from 'jose';
import type { Db } from '../db.ts';
import { nowIso, tx } from '../db.ts';
import { ApiError } from '../http.ts';
import type { Audit } from '../identity/audit.ts';
import type { User } from '../identity/identity.ts';
import { compareVersions } from '../fleet/fleet.ts';
import type { PushHub, PushTarget } from '../fleet/push.ts';
import { KeyRing } from '../keys.ts';
import { readPackage, type Manifest, type PackageLimits } from './package.ts';
import { scanPackage, type ScanResult } from './scanner.ts';
import { diffFiles } from './diff.ts';

export const SCOPES = ['shared', 'group', 'org'] as const;
export type Scope = (typeof SCOPES)[number];
export type State = 'in-review' | 'published' | 'rejected' | 'withdrawn' | 'revoked';

export interface SkillsConfig extends PackageLimits {
  revocationRetentionDays: number;
  allowInsecureDownload: boolean;
}

type Actor = Pick<User, 'subject' | 'userName' | 'groups' | 'adminRoles'>;

interface VersionRow {
  id: number;
  name: string;
  version: string;
  scope: Scope;
  audience: string;
  digest: string;
  size: number;
  manifest: string;
  scan: string;
  state: State;
  author: string;
  source_url: string | null;
  licence: string | null;
  signature: string | null;
  review_comment: string | null;
  reviewed_by: string | null;
  reviewed_at: string | null;
  created_at: string;
  published_at: string | null;
  ended_at: string | null;
  revoke_reason: string | null;
}

const GROUP = /^[a-z0-9][a-z0-9_-]{0,63}$/;
const USER = /^[^\s,]{1,128}$/;
const SHA256 = /^[0-9a-f]{64}$/;
const COLUMNS = 'id, name, version, scope, audience, digest, size, manifest, scan, state, author, source_url, licence, signature, review_comment, reviewed_by, reviewed_at, created_at, published_at, ended_at, revoke_reason';

const lower = (s: string) => s.toLowerCase();
const same = (a: string, b: string) => lower(a) === lower(b);
const isReviewer = (a: Actor) => a.adminRoles.includes('platform-admin') || a.adminRoles.includes('skill-reviewer');

const DOWNLOAD_REDIRECTS = 5;
const DOWNLOAD_TIMEOUT_MS = 30_000;

function httpsUrl(v: string, field: string, allowHttp = false): URL {
  let url: URL;
  try {
    url = new URL(v);
  } catch {
    throw new ApiError(400, 'VALIDATION_FAILED', `The URL ${v} is not valid.`, { field });
  }
  if (url.protocol !== 'https:' && !(allowHttp && url.protocol === 'http:')) throw new ApiError(400, 'VALIDATION_FAILED', `The URL ${url.href} must use HTTPS.`, { field });
  return url;
}

/** Downloads a Skill package: HTTPS only (redirects too), no credentials, cut at the package size limit. */
async function download(start: URL, maxBytes: number, allowHttp: boolean): Promise<Buffer> {
  const failed = (why: string) => new ApiError(400, 'VALIDATION_FAILED', `The package could not be downloaded from ${start.href}: ${why}.`, { field: 'url' });
  const signal = AbortSignal.timeout(DOWNLOAD_TIMEOUT_MS);
  let url = start;
  for (let hop = 0; ; hop++) {
    let res: Response;
    try {
      res = await fetch(url, { redirect: 'manual', signal, headers: { accept: 'application/zip, application/octet-stream;q=0.9, */*;q=0.1' } });
    } catch (err) {
      const e = err as Error & { cause?: { code?: string; message?: string } };
      throw failed(e.name === 'TimeoutError' ? 'it took longer than 30 seconds' : (e.cause?.code ?? e.cause?.message ?? e.message));
    }
    if (res.status >= 300 && res.status < 400 && res.headers.get('location')) {
      await res.body?.cancel();
      if (hop >= DOWNLOAD_REDIRECTS) throw failed('too many redirects');
      const next = new URL(res.headers.get('location')!, url);
      if (next.protocol !== 'https:' && !(allowHttp && next.protocol === 'http:')) throw failed(`it redirects to ${next.href}, which does not use HTTPS`);
      url = next;
      continue;
    }
    if (!res.ok) {
      await res.body?.cancel();
      throw failed(`the server answered HTTP ${res.status}`);
    }
    if (Number(res.headers.get('content-length') ?? 0) > maxBytes) {
      await res.body?.cancel();
      throw new ApiError(413, 'TOO_LARGE', `The package is larger than the limit of ${maxBytes} bytes.`);
    }
    const chunks: Buffer[] = [];
    let size = 0;
    const reader = res.body?.getReader();
    try {
      for (let r = await reader?.read(); r && !r.done; r = await reader!.read()) {
        size += r.value.length;
        if (size > maxBytes) {
          await reader!.cancel();
          throw new ApiError(413, 'TOO_LARGE', `The package is larger than the limit of ${maxBytes} bytes.`);
        }
        chunks.push(Buffer.from(r.value));
      }
    } catch (err) {
      if (err instanceof ApiError) throw err;
      throw failed((err as Error).message);
    }
    return Buffer.concat(chunks);
  }
}

/** The Monday (UTC) of the ISO week that contains `d`, as YYYY-MM-DD. */
export function isoWeek(d = new Date()): string {
  const day = (d.getUTCDay() + 6) % 7;
  return new Date(Date.UTC(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate() - day)).toISOString().slice(0, 10);
}

export class SkillRegistry {
  readonly #db: Db;
  readonly #audit: Audit;
  readonly #push: PushHub;
  readonly #cfg: SkillsConfig;
  readonly #publishScopes: (u: Actor) => string[];
  readonly keys: KeyRing;

  constructor(cfg: SkillsConfig, db: Db, audit: Audit, push: PushHub, publishScopes: (u: Actor) => string[]) {
    this.#cfg = cfg;
    this.#db = db;
    this.#audit = audit;
    this.#push = push;
    this.#publishScopes = publishScopes;
    // Rotated-out skill keys stay published, so that every signature ever issued verifies (Signing Keys).
    this.keys = new KeyRing(db, 'skills', () => 100 * 365 * 86400);
  }

  // ---- reads ----------------------------------------------------------------------------------------------------------

  #row(id: number): VersionRow | undefined {
    return this.#db.prepare(`SELECT ${COLUMNS} FROM skill_versions WHERE id = ?`).get(id) as VersionRow | undefined;
  }

  #mustRow(id: number): VersionRow {
    const r = Number.isInteger(id) ? this.#row(id) : undefined;
    if (!r) throw new ApiError(404, 'NOT_FOUND', `No Skill version ${id}.`);
    return r;
  }

  #byNameVersion(name: string, version: string): VersionRow | undefined {
    return this.#db.prepare(`SELECT ${COLUMNS} FROM skill_versions WHERE name = ? AND version = ?`).get(name, version) as VersionRow | undefined;
  }

  #owner(name: string): string | undefined {
    return (this.#db.prepare('SELECT owner FROM skills WHERE name = ?').get(name) as { owner: string } | undefined)?.owner;
  }

  #summary(r: VersionRow) {
    const m = JSON.parse(r.manifest) as Manifest;
    const scan = JSON.parse(r.scan) as ScanResult;
    return {
      id: r.id, name: r.name, version: r.version, scope: r.scope, audience: JSON.parse(r.audience) as string[], digest: r.digest, size: r.size,
      state: r.state, author: r.author, description: m.description, risk: m.risk, network: m.network, tools: m.tools, requires_mcp: m.requires_mcp,
      findings: scan.findings.length, source_url: r.source_url, licence: r.licence, review_comment: r.review_comment, reviewed_by: r.reviewed_by,
      reviewed_at: r.reviewed_at, created_at: r.created_at, published_at: r.published_at, ended_at: r.ended_at, revoke_reason: r.revoke_reason,
    };
  }

  /** Whether the user is in the audience of a version (shared: named users; group: member Groups; org: everybody). */
  #sees(u: Pick<User, 'userName' | 'groups'>, r: Pick<VersionRow, 'scope' | 'audience'>): boolean {
    if (r.scope === 'org') return true;
    const aud = JSON.parse(r.audience) as string[];
    return r.scope === 'shared' ? aud.some((a) => same(a, u.userName)) : aud.some((g) => u.groups.includes(g));
  }

  #targets(r: Pick<VersionRow, 'scope' | 'audience'>): PushTarget[] {
    if (r.scope === 'org') return [{ everyone: true }];
    const aud = JSON.parse(r.audience) as string[];
    return aud.map((a) => (r.scope === 'shared' ? { user: a } : { group: a }));
  }

  #notify(r: VersionRow, event: string, extra: Record<string, unknown> = {}): void {
    for (const t of this.#targets(r)) this.#push.send('skills-changed', t, { name: r.name, version: r.version, scope: r.scope, event, ...extra });
  }

  #notifyAuthor(r: VersionRow, event: string, extra: Record<string, unknown> = {}): void {
    this.#push.send('skills-changed', { user: r.author }, { name: r.name, version: r.version, scope: r.scope, event, ...extra });
  }

  // ---- publishing -----------------------------------------------------------------------------------------------------

  #audience(actor: Actor, scope: Scope, users: string[], groups: string[]): string[] {
    if (scope === 'org') {
      if (users.length || groups.length) throw new ApiError(400, 'VALIDATION_FAILED', 'An org Skill has no named audience.', { field: 'audience' });
      return [];
    }
    if (scope === 'shared') {
      if (groups.length) throw new ApiError(400, 'VALIDATION_FAILED', 'A shared Skill names users, not Groups.', { field: 'groups' });
      if (!users.length) throw new ApiError(400, 'VALIDATION_FAILED', 'A shared Skill needs at least one user.', { field: 'users' });
      for (const u of users) if (!USER.test(u)) throw new ApiError(400, 'VALIDATION_FAILED', `The user name "${u}" is not valid.`, { field: 'users' });
      return [...new Set(users.map(lower))].sort();
    }
    if (users.length) throw new ApiError(400, 'VALIDATION_FAILED', 'A group Skill names Groups, not users.', { field: 'users' });
    if (!groups.length) throw new ApiError(400, 'VALIDATION_FAILED', 'A group Skill needs at least one Group.', { field: 'groups' });
    for (const g of groups) {
      if (!GROUP.test(g)) throw new ApiError(400, 'VALIDATION_FAILED', `The Group name "${g}" is not valid.`, { field: 'groups' });
      if (!actor.adminRoles.includes('platform-admin') && !actor.groups.includes(g)) {
        throw new ApiError(403, 'NOT_A_MEMBER', `You are not a member of the Group ${g}.`, { field: 'groups', group: g });
      }
    }
    return [...new Set(groups)].sort();
  }

  /**
   * Publishes a version (Skill Storage And Versioning, Sharing Scopes, Publish Audience, Skill Name Ownership). An org
   * version, and every import, waits for review.
   */
  publish(actor: Actor, bytes: Buffer, opts: { scope: unknown; users?: string[]; groups?: string[]; import?: { source_url: string | null; licence: string } }) {
    const scope = opts.scope as Scope;
    if (!SCOPES.includes(scope)) throw new ApiError(400, 'VALIDATION_FAILED', 'scope must be shared, group or org.', { field: 'scope' });
    if (opts.import) {
      if (scope !== 'org') throw new ApiError(400, 'VALIDATION_FAILED', 'An imported Skill goes to org review.', { field: 'scope' });
    } else if (!this.#publishScopes(actor).includes(scope)) {
      this.#audit.write({ category: 'skills', action: 'skill.publish', outcome: 'refused', subject: actor.subject, userName: actor.userName, reason: `scope ${scope} not permitted` });
      throw new ApiError(403, 'SCOPE_NOT_PERMITTED', `Your policy does not allow publishing to the ${scope} scope.`, { scope });
    }
    const audience = this.#audience(actor, scope, opts.users ?? [], opts.groups ?? []);
    const pkg = readPackage(bytes, this.#cfg);
    const m = pkg.manifest;
    const digest = createHash('sha256').update(bytes).digest('hex');
    if (opts.import && !opts.import.licence) {
      const declared = m.frontmatter.license;
      if (typeof declared !== 'string' || !declared.trim()) {
        throw new ApiError(400, 'VALIDATION_FAILED', 'The licence of an imported Skill is required: give one, or declare license in SKILL.md.', { field: 'licence' });
      }
      opts.import.licence = declared.trim();
    }

    const owner = this.#owner(m.name);
    if (owner && !same(owner, actor.userName)) {
      throw new ApiError(409, 'NAME_TAKEN', `The Skill name ${m.name} belongs to ${owner}.`, { name: m.name, owner });
    }
    const existing = this.#byNameVersion(m.name, m.version);
    if (existing) {
      if (existing.digest === digest && existing.scope === scope && existing.audience === JSON.stringify(audience)) return this.#summary(existing);
      throw new ApiError(409, 'VERSION_EXISTS', `Version ${m.version} of ${m.name} already exists and cannot change. Publish a new version number.`, { name: m.name, version: m.version });
    }

    const scan = scanPackage(pkg);
    const secrets = scan.findings.filter((f) => f.kind === 'secret');
    if (secrets.length) {
      const where = secrets.map((f) => ({ file: f.file, line: f.line, rule: f.rule }));
      this.#audit.write({ category: 'skills', action: 'skill.publish', outcome: 'refused', subject: actor.subject, userName: actor.userName, reason: 'secret found', details: { name: m.name, version: m.version, where } });
      throw new ApiError(422, 'SECRET_FOUND', `The Skill contains a secret: ${where.map((w) => `${w.file} line ${w.line} (${w.rule})`).join(', ')}. Remove it and publish again.`, { findings: where });
    }

    const now = nowIso();
    const state: State = scope === 'org' ? 'in-review' : 'published';
    const id = tx(this.#db, () => {
      if (!owner) this.#db.prepare('INSERT INTO skills (name, owner, created_at) VALUES (?, ?, ?)').run(m.name, lower(actor.userName), now);
      return Number(
        this.#db
          .prepare(
            `INSERT INTO skill_versions (name, version, scope, audience, digest, size, bytes, manifest, scan, state, author, source_url, licence, created_at, published_at)
             VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
          )
          .run(
            m.name, m.version, scope, JSON.stringify(audience), digest, bytes.length, bytes, JSON.stringify(m), JSON.stringify(scan), state,
            lower(actor.userName), opts.import?.source_url ?? null, opts.import?.licence ?? null, now, state === 'published' ? now : null,
          ).lastInsertRowid,
      );
    });
    const row = this.#row(id)!;
    this.#audit.write({
      category: 'skills', action: opts.import ? 'skill.import' : 'skill.publish', outcome: 'ok', subject: actor.subject, userName: actor.userName,
      details: { id, name: m.name, version: m.version, scope, audience, digest, state, ...(opts.import ?? {}) },
    });
    if (state === 'published') this.#notify(row, 'published');
    return this.#summary(row);
  }

  /**
   * Public Skill Import: a platform-admin submits an external Skill to org review, either as the package bytes (with an
   * optional source URL) or as the HTTPS URL of the package, which the registry downloads.
   */
  async import(actor: Actor, bytes: Buffer, opts: { url?: string; source_url?: string; licence?: string }) {
    if (!actor.adminRoles.includes('platform-admin')) throw new ApiError(403, 'FORBIDDEN', 'Only a platform-admin can import a public Skill.');
    const lic = opts.licence?.trim() ?? '';
    if (opts.url) {
      if (bytes.length) throw new ApiError(400, 'VALIDATION_FAILED', 'Give either a package or a URL to download, not both.', { field: 'url' });
      const url = httpsUrl(opts.url, 'url', this.#cfg.allowInsecureDownload);
      const pkg = await download(url, this.#cfg.maxPackageBytes, this.#cfg.allowInsecureDownload);
      return this.publish(actor, pkg, { scope: 'org', import: { source_url: url.href, licence: lic } });
    }
    if (!bytes.length) throw new ApiError(400, 'VALIDATION_FAILED', 'Give the Skill package (zip) or a URL to download it from.', { field: 'url' });
    const source = opts.source_url ? httpsUrl(opts.source_url, 'source_url').href : null;
    return this.publish(actor, bytes, { scope: 'org', import: { source_url: source, licence: lic } });
  }

  // ---- review ---------------------------------------------------------------------------------------------------------

  queue() {
    return (this.#db.prepare(`SELECT ${COLUMNS} FROM skill_versions WHERE state = 'in-review' ORDER BY created_at`).all() as unknown as VersionRow[]).map((r) => this.#summary(r));
  }

  /** A version with its scan, files and the diff against the previous version of the Skill (Review Queue). */
  detail(id: number) {
    const r = this.#mustRow(id);
    const bytes = (this.#db.prepare('SELECT bytes FROM skill_versions WHERE id = ?').get(r.id) as { bytes: Uint8Array }).bytes;
    const pkg = readPackage(Buffer.from(bytes), this.#cfg);
    const prev = (this.#db
      .prepare(`SELECT ${COLUMNS} FROM skill_versions WHERE name = ? AND id != ? AND state IN ('published', 'withdrawn', 'revoked')`)
      .all(r.name, r.id) as unknown as VersionRow[])
      .filter((p) => compareVersions(p.version, r.version) < 0)
      .sort((a, b) => compareVersions(b.version, a.version))[0];
    let diff = null;
    if (prev) {
      const prevBytes = (this.#db.prepare('SELECT bytes FROM skill_versions WHERE id = ?').get(prev.id) as { bytes: Uint8Array }).bytes;
      diff = { against: prev.version, files: diffFiles(readPackage(Buffer.from(prevBytes), this.#cfg).files, pkg.files) };
    }
    return {
      ...this.#summary(r),
      scan: JSON.parse(r.scan) as ScanResult,
      frontmatter: pkg.manifest.frontmatter,
      body: pkg.body,
      files: pkg.files.map((f) => ({ path: f.path, size: f.data.length })),
      diff,
    };
  }

  async approve(actor: Actor, id: number, comment: unknown) {
    const r = this.#mustRow(id);
    if (!isReviewer(actor)) throw new ApiError(403, 'FORBIDDEN', 'Only a skill-reviewer or platform-admin can approve a Skill.');
    if (r.state !== 'in-review') throw new ApiError(409, 'NOT_IN_REVIEW', `Version ${r.version} of ${r.name} is ${r.state}, not in review.`);
    if (same(r.author, actor.userName)) {
      this.#audit.write({ category: 'skills', action: 'skill.approve', outcome: 'refused', subject: actor.subject, userName: actor.userName, reason: 'self-review', details: { id } });
      throw new ApiError(403, 'SELF_REVIEW', 'You cannot approve your own Skill version.');
    }
    const key = await this.keys.current();
    const signature = await new SignJWT({ name: r.name, version: r.version, digest: r.digest, scope: r.scope })
      .setProtectedHeader({ alg: 'ES256', kid: key.kid, typ: 'workbench-skill+jwt' })
      .setIssuedAt()
      .sign(key.key);
    const now = nowIso();
    const text = typeof comment === 'string' ? comment.trim() : '';
    const done = this.#db
      .prepare(`UPDATE skill_versions SET state = 'published', signature = ?, review_comment = ?, reviewed_by = ?, reviewed_at = ?, published_at = ? WHERE id = ? AND state = 'in-review'`)
      .run(signature, text || null, lower(actor.userName), now, now, r.id);
    if (done.changes === 0) throw new ApiError(409, 'NOT_IN_REVIEW', 'The version was decided meanwhile.');
    this.#audit.write({ category: 'skills', action: 'skill.approve', outcome: 'ok', subject: actor.subject, userName: actor.userName, details: { id, name: r.name, version: r.version, digest: r.digest, kid: key.kid } });
    const row = this.#row(r.id)!;
    this.#notify(row, 'published');
    this.#notifyAuthor(row, 'approved', text ? { comment: text } : {});
    return this.#summary(row);
  }

  reject(actor: Actor, id: number, comment: unknown) {
    const r = this.#mustRow(id);
    if (!isReviewer(actor)) throw new ApiError(403, 'FORBIDDEN', 'Only a skill-reviewer or platform-admin can reject a Skill.');
    if (r.state !== 'in-review') throw new ApiError(409, 'NOT_IN_REVIEW', `Version ${r.version} of ${r.name} is ${r.state}, not in review.`);
    const text = typeof comment === 'string' ? comment.trim() : '';
    if (!text) throw new ApiError(400, 'VALIDATION_FAILED', 'A rejection needs a comment.', { field: 'comment' });
    const now = nowIso();
    this.#db
      .prepare(`UPDATE skill_versions SET state = 'rejected', review_comment = ?, reviewed_by = ?, reviewed_at = ?, ended_at = ? WHERE id = ?`)
      .run(text, lower(actor.userName), now, now, r.id);
    this.#audit.write({ category: 'skills', action: 'skill.reject', outcome: 'ok', subject: actor.subject, userName: actor.userName, details: { id, name: r.name, version: r.version } });
    const row = this.#row(r.id)!;
    this.#notifyAuthor(row, 'rejected', { comment: text });
    return this.#summary(row);
  }

  // ---- withdrawal and revocation --------------------------------------------------------------------------------------

  /** Author Withdrawal: no longer distributed; not a revocation. */
  withdraw(actor: Actor, name: string, version: string) {
    const r = this.#byNameVersion(name, version);
    if (!r || !same(r.author, actor.userName)) throw new ApiError(404, 'NOT_FOUND', `You have no version ${version} of ${name}.`);
    if (r.state !== 'published' && r.state !== 'in-review') throw new ApiError(409, 'NOT_ACTIVE', `Version ${version} of ${name} is ${r.state}.`);
    this.#db.prepare(`UPDATE skill_versions SET state = 'withdrawn', ended_at = ? WHERE id = ?`).run(nowIso(), r.id);
    this.#audit.write({ category: 'skills', action: 'skill.withdraw', outcome: 'ok', subject: actor.subject, userName: actor.userName, details: { id: r.id, name, version } });
    if (r.state === 'published') this.#notify(r, 'withdrawn');
    return this.#summary(this.#row(r.id)!);
  }

  /** Skill Revocation: one version, or every published version of the name. */
  revoke(actor: Actor, name: string, version: string | undefined, reason: unknown) {
    const why = typeof reason === 'string' ? reason.trim() : '';
    if (!why) throw new ApiError(400, 'VALIDATION_FAILED', 'A revocation needs a reason.', { field: 'reason' });
    const rows = version
      ? [this.#byNameVersion(name, version)].filter((r): r is VersionRow => !!r && r.state === 'published')
      : (this.#db.prepare(`SELECT ${COLUMNS} FROM skill_versions WHERE name = ? AND state = 'published'`).all(name) as unknown as VersionRow[]);
    if (!rows.length) throw new ApiError(404, 'NOT_FOUND', version ? `No published version ${version} of ${name}.` : `No published version of ${name}.`);
    for (const r of rows) {
      const allowed = isReviewer(actor) || (same(r.author, actor.userName) && r.scope !== 'org');
      if (!allowed) {
        this.#audit.write({ category: 'skills', action: 'skill.revoke', outcome: 'refused', subject: actor.subject, userName: actor.userName, details: { name, version: r.version } });
        throw new ApiError(403, 'FORBIDDEN', r.scope === 'org' ? 'Only a skill-reviewer or platform-admin can revoke an org Skill.' : `You cannot revoke ${name}.`);
      }
    }
    const now = nowIso();
    tx(this.#db, () => {
      for (const r of rows) this.#db.prepare(`UPDATE skill_versions SET state = 'revoked', ended_at = ?, revoke_reason = ? WHERE id = ?`).run(now, why, r.id);
    });
    this.#audit.write({ category: 'skills', action: 'skill.revoke', outcome: 'ok', subject: actor.subject, userName: actor.userName, reason: why, details: { name, versions: rows.map((r) => r.version) } });
    // One skill-revoked push per audience target, carrying the versions that target received.
    const byTarget = new Map<string, { target: PushTarget; versions: { version: string; digest: string }[] }>();
    for (const r of rows) {
      for (const t of this.#targets(r)) {
        const k = JSON.stringify(t);
        const e = byTarget.get(k) ?? { target: t, versions: [] };
        e.versions.push({ version: r.version, digest: r.digest });
        byTarget.set(k, e);
      }
    }
    for (const { target, versions } of byTarget.values()) this.#push.send('skill-revoked', target, { name, versions, reason: why });
    return { name, revoked: rows.map((r) => ({ version: r.version, digest: r.digest, scope: r.scope })), reason: why };
  }

  // ---- distribution ---------------------------------------------------------------------------------------------------

  /** Skill Distribution: the highest visible published version per name and scope, and the recent revocations. */
  syncFor(u: Pick<User, 'userName' | 'groups'>) {
    const rows = this.#db.prepare(`SELECT ${COLUMNS} FROM skill_versions WHERE state = 'published'`).all() as unknown as VersionRow[];
    const best = new Map<string, VersionRow>();
    for (const r of rows) {
      if (!this.#sees(u, r)) continue;
      const k = `${r.name}\u0000${r.scope}`;
      const cur = best.get(k);
      if (!cur || compareVersions(r.version, cur.version) > 0) best.set(k, r);
    }
    const skills = [...best.values()]
      .sort((a, b) => a.name.localeCompare(b.name) || a.scope.localeCompare(b.scope))
      .map((r) => {
        const m = JSON.parse(r.manifest) as Manifest;
        const aud = JSON.parse(r.audience) as string[];
        return {
          name: r.name, version: r.version, scope: r.scope, digest: r.digest, size: r.size, author: r.author, description: m.description,
          risk: m.risk, tools: m.tools, requires_mcp: m.requires_mcp, published_at: r.published_at,
          ...(r.scope === 'group' ? { groups: aud.filter((g) => u.groups.includes(g)) } : {}),
          ...(r.signature ? { signature: r.signature } : {}),
        };
      });
    const since = new Date(Date.now() - this.#cfg.revocationRetentionDays * 86400_000).toISOString();
    const revocations = (this.#db
      .prepare(`SELECT ${COLUMNS} FROM skill_versions WHERE state = 'revoked' AND ended_at >= ? ORDER BY ended_at`)
      .all(since) as unknown as VersionRow[])
      .filter((r) => this.#sees(u, r))
      .map((r) => ({ name: r.name, version: r.version, scope: r.scope, digest: r.digest, revoked_at: r.ended_at, reason: r.revoke_reason }));
    return { skills, revocations, keys_url: '/.well-known/workbench-skill-keys.json' };
  }

  /** Content by digest: for users who may see a published version with it, its author, and reviewers. */
  content(u: Actor, digest: string): Buffer | null {
    const d = digest.toLowerCase();
    if (!SHA256.test(d)) return null;
    const rows = this.#db.prepare(`SELECT ${COLUMNS} FROM skill_versions WHERE digest = ?`).all(d) as unknown as VersionRow[];
    const ok = rows.some((r) => (r.state === 'published' && this.#sees(u, r)) || same(r.author, u.userName) || (isReviewer(u) && r.state !== 'revoked'));
    if (!ok) return null;
    const row = this.#db.prepare('SELECT bytes FROM skill_versions WHERE digest = ? LIMIT 1').get(d) as { bytes: Uint8Array };
    return Buffer.from(row.bytes);
  }

  /** The author's own Skills and versions. */
  mine(u: Pick<User, 'userName'>) {
    return (this.#db.prepare(`SELECT ${COLUMNS} FROM skill_versions WHERE author = ? ORDER BY name, id`).all(lower(u.userName)) as unknown as VersionRow[]).map((r) => this.#summary(r));
  }

  /** Every Skill name with its versions (admin console). */
  all() {
    return (this.#db.prepare(`SELECT ${COLUMNS} FROM skill_versions ORDER BY name, id`).all() as unknown as VersionRow[]).map((r) => this.#summary(r));
  }

  // ---- usage ----------------------------------------------------------------------------------------------------------

  /** Usage Reporting: adds counts to the current ISO week; stores no user or installation. */
  reportUsage(body: unknown) {
    const items = (body as { skills?: unknown })?.skills;
    if (!Array.isArray(items) || items.length > 500) throw new ApiError(400, 'VALIDATION_FAILED', 'skills must be a list of at most 500 counts.', { field: 'skills' });
    const count = (v: unknown, field: string) => {
      if (v === undefined) return 0;
      if (!Number.isInteger(v) || (v as number) < 0 || (v as number) > 100000) throw new ApiError(400, 'VALIDATION_FAILED', `${field} must be a whole number from 0 to 100000.`, { field });
      return v as number;
    };
    const week = isoWeek();
    let accepted = 0;
    tx(this.#db, () => {
      for (const it of items as Record<string, unknown>[]) {
        const name = typeof it?.name === 'string' ? it.name : '';
        const installs = count(it?.installs, 'installs');
        const runs = count(it?.runs, 'runs');
        const failures = count(it?.failures, 'failures');
        if (!this.#owner(name)) continue;
        this.#db
          .prepare(
            `INSERT INTO skill_usage_weekly (name, week, installs, runs, failures) VALUES (?, ?, ?, ?, ?)
             ON CONFLICT (name, week) DO UPDATE SET installs = installs + excluded.installs, runs = runs + excluded.runs, failures = failures + excluded.failures`,
          )
          .run(name, week, installs, runs, failures);
        accepted++;
      }
    });
    return { week, accepted };
  }

  /** Usage Counts For Authors: weekly counts of the author's own Skill, with no user names. */
  usage(u: Pick<User, 'userName'>, name: string) {
    const owner = this.#owner(name);
    if (!owner || !same(owner, u.userName)) throw new ApiError(404, 'NOT_FOUND', `You have no Skill ${name}.`);
    const weeks = (this.#db.prepare('SELECT week, installs, runs, failures FROM skill_usage_weekly WHERE name = ? ORDER BY week DESC').all(name) as {
      week: string; installs: number; runs: number; failures: number;
    }[]).map((w) => ({ ...w, failure_rate: w.runs ? Math.round((w.failures / w.runs) * 1000) / 1000 : 0 }));
    return { name, weeks };
  }
}

