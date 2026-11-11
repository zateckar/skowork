// control-plane/team-knowledge: versioned team knowledge publications (improve-skowork-and-knowledge-control design D9).
// A publication is owned by the identity-provider issuer and stable subject, written only through the revision ledger,
// and read or searched only through one authorization relation that re-evaluates the owner's current Group membership,
// sharing policy and revocation and the reader's current Groups on every request.
import { Router, type NextFunction, type Request, type Response } from 'express';
import { randomUUID } from 'node:crypto';
import type { Config } from '../config.ts';
import type { Db } from '../db.ts';
import { nowIso } from '../db.ts';
import { ApiError } from '../http.ts';
import type { Audit } from '../identity/audit.ts';
import { EVERYONE, type User } from '../identity/identity.ts';
import { publicationChunks, removePublicationContent, removePublicationEntry, replacePublicationContent } from './publication-content.ts';
import { parsePublicationInput, requirePermittedPublicationSources, PUBLICATION_SOURCE_KINDS, type PublicationInput } from './publication-input.ts';
import { commitPublicationRevision, publicationKey, type PublicationAction } from './publication-ledger.ts';
import type { Embedder } from './team.ts';

/** The schema versions this server implements; advertised only when publications are enabled. */
export const PUBLICATION_VERSIONS = [1] as const;
/** The request header naming the mutation schema version a client writes. */
export const PUBLICATION_VERSION_HEADER = 'skowork-publication-version';

const EMBED_BATCH = 64;
const TOP_K = 50;
const RRF_K = 60;
/** Entries per page of a direct read; a publication has at most 500. */
const READ_PAGE = 100;
const READ_PAGE_MAX = 500;
const INVALID_ID = /[^A-Za-z0-9._:-]/;

export interface PublicationDeps {
  embed: Embedder;
  /** Current Groups of a username from control-plane assignments and the given identity-provider roles. */
  groupsFor: (userName: string, idpRoles: string[]) => string[];
  /** The compiled effective policy for those Groups. */
  policyFor: (groups: string[], userName: string) => Map<string, unknown>;
}

type Authority = { permitted: Set<string>; kinds: Set<string> };
interface Owner { issuer: string; subject: string; user_name: string; idp_roles: string; roles_expire_at: number }
interface ContentRow { issuer: string; subject: string; publication_id: string; title: string; size: number; embed_model: string | null; updated_at: string; revision: number; public_key: string }
/** One publication as a reader may see it: the audience they share with it and the entries current policy permits. */
interface Visible { row: ContentRow; owner: string; groups: string[]; kinds: Set<string> }

function quantise(v: number[]): Buffer {
  const norm = Math.sqrt(v.reduce((s, x) => s + x * x, 0)) || 1;
  const b = Buffer.alloc(v.length);
  for (let i = 0; i < v.length; i++) b.writeInt8(Math.max(-127, Math.min(127, Math.round((v[i]! / norm) * 127))), i);
  return b;
}
function dot(a: Buffer, b: Buffer): number {
  let s = 0;
  for (let i = 0; i < a.length; i++) s += a.readInt8(i) * b.readInt8(i);
  return s;
}
function ftsQuery(q: string): string | null {
  const words = [...new Set(q.toLowerCase().match(/[\p{L}\p{N}]{2,}/gu) ?? [])].slice(0, 32);
  return words.length ? words.map((w) => `"${w}"`).join(' OR ') : null;
}
function list(value: unknown): string[] {
  return Array.isArray(value) ? value.map(String) : [];
}
function checkId(value: string, what: string): string {
  if (!value || value.length > 200 || INVALID_ID.test(value)) throw new ApiError(400, 'INVALID_REQUEST', `Invalid ${what}.`);
  return value;
}
function fence(body: Record<string, unknown>): { expected: number; operation: string } {
  const expected = body.expected_revision;
  const operation = body.operation_id;
  if (!Number.isSafeInteger(expected) || (expected as number) < 0 || typeof operation !== 'string')
    throw new ApiError(400, 'INVALID_REQUEST', 'expected_revision and operation_id are required.');
  return { expected: expected as number, operation: checkId(operation, 'operation id') };
}

export class TeamPublications {
  readonly #cfg: Config;
  readonly #db: Db;
  readonly #audit: Audit;
  readonly #deps: PublicationDeps;

  constructor(cfg: Config, db: Db, audit: Audit, deps: PublicationDeps) {
    this.#cfg = cfg;
    this.#db = db;
    this.#audit = audit;
    this.#deps = deps;
  }

  get enabled(): boolean {
    return this.#cfg.teamKnowledge.publications;
  }

  capabilities() {
    const versions = this.enabled ? [...PUBLICATION_VERSIONS] : [];
    return { publication_read_versions: versions, publication_mutation_versions: versions };
  }

  #requireEnabled() {
    if (!this.enabled) throw new ApiError(404, 'NOT_FOUND', 'No such endpoint.');
  }

  /** A mutation names a version this server advertises, or nothing is changed. */
  #requireVersion(header: string | undefined) {
    this.#requireEnabled();
    if (!header || !(PUBLICATION_VERSIONS as readonly number[]).includes(Number(header)) || String(Number(header)) !== header.trim())
      throw new ApiError(400, 'UNSUPPORTED_VERSION', `Publication changes need the ${PUBLICATION_VERSION_HEADER} header with a supported version.`, {
        supported: [...PUBLICATION_VERSIONS],
      });
  }

  // ---- current authority ------------------------------------------------------------------------------------------

  /** The caller as of now: an expired token or a revoked user is refused, and Groups come from current assignments. */
  #current(user: User): User {
    if (user.tokenExpiresAt + this.#cfg.identity.clockSkewS <= Date.now() / 1000)
      throw new ApiError(401, 'UNAUTHORIZED', 'Your session expired while the request was being prepared.');
    if (this.#denied(user.userName)) throw new ApiError(403, 'FORBIDDEN', 'This user is no longer authorized.');
    if (!user.issuer || !user.subject) throw new ApiError(403, 'FORBIDDEN', 'Publications need an identity-provider subject.');
    return { ...user, groups: this.#deps.groupsFor(user.userName, user.idpRoles) };
  }

  #denied(userName: string): boolean {
    return !!this.#db.prepare("SELECT 1 FROM deny_list WHERE kind = 'user' AND lower(value) = lower(?)").get(userName);
  }

  /** Records who the owner is now, outside any write transaction, so a refused write cannot undo a newer snapshot. */
  #recordOwner(user: User) {
    this.#db.prepare(`INSERT INTO knowledge_publication_owners (issuer, subject, user_name, idp_roles, roles_expire_at, updated_at)
      VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT (issuer, subject) DO UPDATE SET user_name = excluded.user_name, idp_roles = excluded.idp_roles,
      roles_expire_at = excluded.roles_expire_at, updated_at = excluded.updated_at
      WHERE excluded.roles_expire_at >= knowledge_publication_owners.roles_expire_at OR excluded.user_name <> knowledge_publication_owners.user_name`)
      .run(user.issuer, user.subject, user.userName, JSON.stringify([...user.idpRoles].sort()), user.tokenExpiresAt, nowIso());
  }

  /**
   * What an owner may publish now: the Groups policy.knowledge.sharing.groups permits among their current Groups (`*`
   * is every Group but everyone, which must be named), and the Source kinds policy.knowledge.sharing.derived_sources
   * permits. Role-derived Groups count only while the roles' token has not expired.
   */
  #authority(userName: string, idpRoles: string[], rolesExpireAt: number): Authority {
    const fresh = rolesExpireAt + this.#cfg.identity.clockSkewS > Date.now() / 1000;
    const groups = this.#deps.groupsFor(userName, fresh ? idpRoles : []);
    const policy = this.#deps.policyFor(groups, userName);
    const member = new Set(groups);
    const permitted = new Set<string>();
    for (const g of list(policy.get('knowledge.sharing.groups'))) {
      if (g === '*') for (const m of member) if (m !== EVERYONE) permitted.add(m);
      if (member.has(g)) permitted.add(g);
    }
    const kinds = new Set(policy.has('knowledge.sharing.derived_sources') ? list(policy.get('knowledge.sharing.derived_sources')) : ['files', 'notes']);
    return { permitted, kinds };
  }

  #ownerAuthority(owner: Owner): Authority | null {
    if (this.#denied(owner.user_name)) return null;
    return this.#authority(owner.user_name, JSON.parse(owner.idp_roles) as string[], owner.roles_expire_at);
  }

  #companyFor(permitted: Set<string>): string | null {
    const company = this.#cfg.teamKnowledge.companyGroup;
    return company && permitted.has(company) ? company : null;
  }

  // ---- the authorization relation ---------------------------------------------------------------------------------

  /** Every publication the reader may see now, with the audience they share and the Source kinds still permitted. */
  #visible(reader: User): Map<string, Visible> {
    const readerGroups = new Set(reader.groups ?? []);
    const out = new Map<string, Visible>();
    const rows = this.#db.prepare(`SELECT c.*, l.revision, l.public_key FROM knowledge_publication_content c
      JOIN knowledge_publication_ledger l USING (issuer, subject, publication_id) WHERE l.admin_blocked = 0`).all() as unknown as ContentRow[];
    const authorities = new Map<string, Authority | null>();
    for (const row of rows) {
      const ownerKey = JSON.stringify([row.issuer, row.subject]);
      const owner = this.#db.prepare('SELECT * FROM knowledge_publication_owners WHERE issuer = ? AND subject = ?').get(row.issuer, row.subject) as Owner | undefined;
      if (!owner) continue;
      if (!authorities.has(ownerKey)) authorities.set(ownerKey, this.#ownerAuthority(owner));
      const authority = authorities.get(ownerKey);
      if (!authority) continue;
      const audience = (this.#db.prepare('SELECT group_name FROM knowledge_publication_groups WHERE issuer = ? AND subject = ? AND publication_id = ? ORDER BY position')
        .all(row.issuer, row.subject, row.publication_id) as { group_name: string }[]).map((g) => g.group_name);
      const groups = audience.filter((g) => authority.permitted.has(g) && readerGroups.has(g));
      if (groups.length) out.set(row.public_key, { row, owner: owner.user_name, groups, kinds: authority.kinds });
    }
    return out;
  }

  /** Entries of a visible publication whose every declared Source kind the owner may still publish. */
  #entries(v: Visible) {
    const rows = this.#db.prepare(`SELECT entry_id, text, source_kinds FROM knowledge_publication_entries
      WHERE issuer = ? AND subject = ? AND publication_id = ? ORDER BY position`).all(v.row.issuer, v.row.subject, v.row.publication_id) as
      { entry_id: string; text: string; source_kinds: string }[];
    return rows.filter((e) => (JSON.parse(e.source_kinds) as string[]).every((k) => v.kinds.has(k)));
  }

  #evidence(v: Visible, entryId: string) {
    return (this.#db.prepare(`SELECT evidence_id AS id, title, text FROM knowledge_publication_evidence
      WHERE issuer = ? AND subject = ? AND publication_id = ? AND entry_id = ? ORDER BY position`)
      .all(v.row.issuer, v.row.subject, v.row.publication_id, entryId) as { id: string; title: string; text: string }[]).map((e) => ({ ...e }));
  }

  #provenance(v: Visible) {
    return { publication: v.row.public_key, owner: v.owner, groups: v.groups, revision: v.row.revision, title: v.row.title, updated_at: v.row.updated_at };
  }

  // ---- owner operations -------------------------------------------------------------------------------------------

  /** The owner's publications, including withdrawn and removed ones, with what they may publish now. */
  mine(user: User) {
    this.#requireEnabled();
    const current = this.#current(user);
    this.#recordOwner(current);
    const authority = this.#authority(current.userName, current.idpRoles, current.tokenExpiresAt);
    const rows = this.#db.prepare(`SELECT l.publication_id, l.revision, l.admin_blocked, l.public_key, c.title, c.size, c.updated_at,
        (SELECT count(*) FROM knowledge_publication_entries e WHERE e.issuer = l.issuer AND e.subject = l.subject AND e.publication_id = l.publication_id) AS entries,
        r.reason, r.removed_at
      FROM knowledge_publication_ledger l
      LEFT JOIN knowledge_publication_content c USING (issuer, subject, publication_id)
      LEFT JOIN knowledge_publication_removals r USING (issuer, subject, publication_id)
      WHERE l.issuer = ? AND l.subject = ? ORDER BY l.publication_id`).all(current.issuer, current.subject) as {
      publication_id: string; revision: number; admin_blocked: number; public_key: string; title: string | null; size: number | null;
      updated_at: string | null; entries: number; reason: string | null; removed_at: string | null;
    }[];
    return {
      permitted_groups: [...authority.permitted].sort(),
      company_group: this.#companyFor(authority.permitted),
      derived_sources: PUBLICATION_SOURCE_KINDS.filter((k) => authority.kinds.has(k)),
      publications: rows.map((r) => ({
        publication_id: r.publication_id, publication: r.public_key, revision: r.revision,
        state: r.admin_blocked ? 'removed' : r.title === null ? 'withdrawn' : 'published',
        title: r.title, size: r.size ?? 0, entries: Number(r.entries), updated_at: r.updated_at,
        groups: (this.#db.prepare('SELECT group_name FROM knowledge_publication_groups WHERE issuer = ? AND subject = ? AND publication_id = ? ORDER BY position')
          .all(current.issuer, current.subject, r.publication_id) as { group_name: string }[]).map((g) => g.group_name),
        removal: r.admin_blocked && r.reason ? { reason: r.reason, removed_at: r.removed_at } : null,
      })),
    };
  }

  /** Publish or replace a whole revision after an expected-revision check; a retry of the same operation is idempotent. */
  async publish(user: User, publicationId: string, version: string | undefined, raw: unknown) {
    this.#requireVersion(version);
    checkId(publicationId, 'publication id');
    const input = parsePublicationInput(raw, this.#cfg.teamKnowledge.maxItemBytes);
    const first = this.#current(user);
    this.#recordOwner(first);
    const size = Buffer.byteLength(JSON.stringify({ title: input.title, entries: input.entries }), 'utf8');
    this.#authorizePublish(first, input, publicationId, size);
    // Embeddings are computed outside the write transaction; everything is checked again before the commit.
    const chunks = input.entries.flatMap((entry) => publicationChunks(entry.text).map((text, position) => ({ entry: entry.id, position, text })));
    let embedded: { model: string; vectors: number[][] } | null = null;
    {
      const vectors: number[][] = [];
      let model: string | null = null;
      for (let i = 0; i < chunks.length; i += EMBED_BATCH) {
        const r = await this.#deps.embed(first, chunks.slice(i, i + EMBED_BATCH).map((c) => c.text));
        if (!r || (model && r.model !== model)) {
          model = null;
          break;
        }
        model = r.model;
        vectors.push(...r.vectors);
      }
      if (model && vectors.length === chunks.length) embedded = { model, vectors };
    }
    const owner = { issuer: first.issuer, subject: first.subject, publicationId };
    const result = commitPublicationRevision(this.#db, {
      ...owner, operationId: input.operation_id, expectedRevision: input.expected_revision, action: 'publish',
      requestEncoding: JSON.stringify([publicationId, input]),
    }, () => {
      this.#authorizePublish(this.#current(user), input, publicationId, size);
    }, () => {
      replacePublicationContent(this.#db, owner, input);
      this.#db.prepare('UPDATE knowledge_publication_content SET size = ?, embed_model = ?, updated_at = ? WHERE issuer = ? AND subject = ? AND publication_id = ?')
        .run(size, embedded?.model ?? null, nowIso(), owner.issuer, owner.subject, publicationId);
      if (embedded) {
        const set = this.#db.prepare('UPDATE knowledge_publication_chunks SET vector = ? WHERE issuer = ? AND subject = ? AND publication_id = ? AND entry_id = ? AND position = ?');
        chunks.forEach((c, i) => set.run(quantise(embedded!.vectors[i]!), owner.issuer, owner.subject, publicationId, c.entry, c.position));
      }
      this.#db.prepare('DELETE FROM knowledge_publication_removals WHERE issuer = ? AND subject = ? AND publication_id = ?').run(owner.issuer, owner.subject, publicationId);
    });
    this.#record(first, 'knowledge.publication.publish', owner, result.committedRevision, input.groups, result.replayed);
    return { publication_id: publicationId, publication: publicationKey(owner.issuer, owner.subject, publicationId), revision: result.committedRevision,
      current_revision: result.currentRevision, replayed: result.replayed };
  }

  /** Audience, Source kinds and quota, from current membership and policy. */
  #authorizePublish(user: User, input: PublicationInput, publicationId: string, size: number) {
    const authority = this.#authority(user.userName, user.idpRoles, user.tokenExpiresAt);
    const refused = input.groups.filter((g) => !authority.permitted.has(g));
    if (refused.length)
      throw new ApiError(403, 'GROUP_NOT_PERMITTED', 'You cannot publish to one or more of these Groups: you are not a member, or policy.knowledge.sharing.groups does not permit it.', { groups: refused });
    requirePermittedPublicationSources(input, authority.kinds);
    const items = (this.#db.prepare(`SELECT COALESCE(SUM(size), 0) AS s FROM team_items
      WHERE (owner_issuer = ? AND owner_subject = ?) OR (owner_subject IS NULL AND owner = ?)`).get(user.issuer, user.subject, user.userName) as { s: number }).s;
    const others = (this.#db.prepare('SELECT COALESCE(SUM(size), 0) AS s FROM knowledge_publication_content WHERE issuer = ? AND subject = ? AND publication_id <> ?')
      .get(user.issuer, user.subject, publicationId) as { s: number }).s;
    if (Number(items) + Number(others) + size > this.#cfg.teamKnowledge.quotaBytes)
      throw new ApiError(413, 'QUOTA_EXCEEDED', 'Your shared knowledge is at its size limit. Withdraw or unshare something first.');
  }

  /** Withdraw the whole publication, or one entry; the revision advances so a delayed publish cannot restore it. */
  withdraw(user: User, publicationId: string, entryId: string | null, version: string | undefined, body: Record<string, unknown>) {
    this.#requireVersion(version);
    checkId(publicationId, 'publication id');
    if (entryId !== null) checkId(entryId, 'entry id');
    const { expected, operation } = fence(body);
    const first = this.#current(user);
    this.#recordOwner(first);
    const owner = { issuer: first.issuer, subject: first.subject, publicationId };
    const result = commitPublicationRevision(this.#db, {
      ...owner, operationId: operation, expectedRevision: expected, action: 'withdraw',
      requestEncoding: JSON.stringify([publicationId, entryId]),
    }, () => {
      this.#current(user);
    }, () => {
      if (entryId === null) removePublicationContent(this.#db, owner);
      else if (!removePublicationEntry(this.#db, owner, entryId)) throw new ApiError(404, 'NOT_FOUND', 'The publication has no such entry.');
    });
    this.#record(first, entryId === null ? 'knowledge.publication.withdraw' : 'knowledge.publication.withdraw-entry', owner, result.committedRevision, null,
      result.replayed, entryId === null ? {} : { entry_id: entryId });
    return { publication_id: publicationId, revision: result.committedRevision, current_revision: result.currentRevision, replayed: result.replayed };
  }

  // ---- readers ------------------------------------------------------------------------------------------------------

  /** Direct read of one publication the caller may see: only its permitted entries and their approved excerpts. */
  read(user: User, key: string, offsetRaw?: unknown, limitRaw?: unknown) {
    this.#requireEnabled();
    const v = this.#visible(this.#current(user)).get(key);
    // Not found and not permitted look the same, so a reader learns nothing about publications they cannot see.
    if (!v) throw new ApiError(404, 'NOT_FOUND', 'No such publication.');
    const offset = Math.max(0, Number.parseInt(String(offsetRaw ?? '0'), 10) || 0);
    const limit = Math.min(READ_PAGE_MAX, Math.max(1, Number.parseInt(String(limitRaw ?? READ_PAGE), 10) || READ_PAGE));
    const all = this.#entries(v);
    const page = all.slice(offset, offset + limit);
    return {
      ...this.#provenance(v), total_entries: all.length, next_offset: offset + page.length < all.length ? offset + page.length : null,
      entries: page.map((e) => ({ entry_id: e.entry_id, text: e.text, source_kinds: JSON.parse(e.source_kinds), evidence: this.#evidence(v, e.entry_id) })),
    };
  }

  /** Publication passages for the caller, full text and vectors fused, with their provenance and approved evidence. */
  async search(user: User, body: Record<string, unknown>) {
    this.#requireEnabled();
    const q = typeof body.query === 'string' ? body.query.trim().slice(0, 2000) : '';
    if (!q) throw new ApiError(400, 'INVALID_REQUEST', 'query is required.');
    const limit = Math.min(50, Math.max(1, Number.isInteger(body.limit) ? (body.limit as number) : 10));
    return { passages: await this.passages(user, q, limit) };
  }

  /** Shared by publication search and the compatible item search; checks authority again after the embedding call. */
  async passages(user: User, q: string, limit: number) {
    const reader = this.#current(user);
    const visible = this.#visible(reader);
    if (!visible.size) return [];
    const allowed = new Map<string, Visible>();
    for (const v of visible.values()) for (const e of this.#entries(v)) allowed.set(JSON.stringify([v.row.issuer, v.row.subject, v.row.publication_id, e.entry_id]), v);
    const chunkKey = (c: { issuer: string; subject: string; publication_id: string; entry_id: string }) => JSON.stringify([c.issuer, c.subject, c.publication_id, c.entry_id]);
    const ranks: Map<number, number>[] = [];
    const fts = ftsQuery(q);
    if (fts) {
      const rows = this.#db.prepare(`SELECT c.id, c.issuer, c.subject, c.publication_id, c.entry_id FROM knowledge_publication_chunks_fts f
        JOIN knowledge_publication_chunks c ON c.id = f.rowid WHERE knowledge_publication_chunks_fts MATCH ? ORDER BY bm25(knowledge_publication_chunks_fts) LIMIT 500`)
        .all(fts) as { id: number; issuer: string; subject: string; publication_id: string; entry_id: string }[];
      ranks.push(new Map(rows.filter((r) => allowed.has(chunkKey(r))).slice(0, TOP_K).map((r, i) => [r.id, i])));
    }
    const models = new Set([...visible.values()].map((v) => v.row.embed_model).filter((m): m is string => !!m));
    if (models.size) {
      const e = await this.#deps.embed(reader, [q]);
      if (e && models.has(e.model)) {
        const qv = quantise(e.vectors[0]!);
        const rows = this.#db.prepare(`SELECT c.id, c.issuer, c.subject, c.publication_id, c.entry_id, c.vector FROM knowledge_publication_chunks c
          JOIN knowledge_publication_content p USING (issuer, subject, publication_id) WHERE p.embed_model = ? AND c.vector IS NOT NULL`)
          .all(e.model) as { id: number; issuer: string; subject: string; publication_id: string; entry_id: string; vector: Uint8Array }[];
        const scored = rows.filter((r) => allowed.has(chunkKey(r)) && r.vector.length === qv.length)
          .map((r) => ({ id: r.id, s: dot(qv, Buffer.from(r.vector)) })).sort((a, b) => b.s - a.s).slice(0, TOP_K);
        ranks.push(new Map(scored.map((r, i) => [r.id, i])));
      }
    }
    const fused = new Map<number, number>();
    for (const m of ranks) for (const [id, rank] of m) fused.set(id, (fused.get(id) ?? 0) + 1 / (RRF_K + rank + 1));
    // The embedding call yields to other requests: authority and withdrawals are checked again before anything is returned.
    const still = this.#visible(this.#current(user));
    const passages = [];
    for (const [chunkId, score] of [...fused.entries()].sort((a, b) => b[1] - a[1])) {
      const c = this.#db.prepare('SELECT * FROM knowledge_publication_chunks WHERE id = ?').get(chunkId) as
        { issuer: string; subject: string; publication_id: string; entry_id: string; position: number; text: string } | undefined;
      if (!c) continue;
      const v = still.get(publicationKey(c.issuer, c.subject, c.publication_id));
      if (!v || !this.#entries(v).some((e) => e.entry_id === c.entry_id)) continue;
      passages.push({ ...this.#provenance(v), entry_id: c.entry_id, chunk: c.position, text: c.text, score: Number(score.toFixed(5)), evidence: this.#evidence(v, c.entry_id) });
      if (passages.length >= limit) break;
    }
    return passages;
  }

  // ---- administration ---------------------------------------------------------------------------------------------

  adminList() {
    const rows = this.#db.prepare(`SELECT l.issuer, l.subject, l.publication_id, l.public_key, l.revision, l.admin_blocked, o.user_name, c.title, c.size, c.updated_at,
        (SELECT count(*) FROM knowledge_publication_entries e WHERE e.issuer = l.issuer AND e.subject = l.subject AND e.publication_id = l.publication_id) AS entries
      FROM knowledge_publication_ledger l LEFT JOIN knowledge_publication_content c USING (issuer, subject, publication_id)
      LEFT JOIN knowledge_publication_owners o USING (issuer, subject) ORDER BY c.updated_at DESC`).all() as {
      issuer: string; subject: string; publication_id: string; public_key: string; revision: number; admin_blocked: number; user_name: string | null; title: string | null; size: number | null; updated_at: string | null; entries: number;
    }[];
    return {
      publications: rows.map((r) => ({
        publication: r.public_key, owner: r.user_name, revision: r.revision, state: r.admin_blocked ? 'removed' : r.title === null ? 'withdrawn' : 'published',
        groups: (this.#db.prepare('SELECT group_name FROM knowledge_publication_groups WHERE issuer = ? AND subject = ? AND publication_id = ? ORDER BY position')
          .all(r.issuer, r.subject, r.publication_id) as { group_name: string }[]).map((g) => g.group_name),
        title: r.title, size: r.size ?? 0, entries: Number(r.entries), updated_at: r.updated_at,
      })),
    };
  }

  /** The exact published revision, for inspection by an administrator. */
  adminRead(key: string) {
    const row = this.#byKey(key);
    const content = this.#db.prepare(`SELECT c.*, l.revision, l.public_key FROM knowledge_publication_content c JOIN knowledge_publication_ledger l USING (issuer, subject, publication_id)
      WHERE l.public_key = ?`).get(key) as unknown as ContentRow | undefined;
    const owner = this.#db.prepare('SELECT user_name FROM knowledge_publication_owners WHERE issuer = ? AND subject = ?').get(row.issuer, row.subject) as { user_name: string } | undefined;
    const groups = (this.#db.prepare('SELECT group_name FROM knowledge_publication_groups WHERE issuer = ? AND subject = ? AND publication_id = ? ORDER BY position')
      .all(row.issuer, row.subject, row.publication_id) as { group_name: string }[]).map((g) => g.group_name);
    const entries = content
      ? (this.#db.prepare('SELECT entry_id, text, source_kinds FROM knowledge_publication_entries WHERE issuer = ? AND subject = ? AND publication_id = ? ORDER BY position')
        .all(row.issuer, row.subject, row.publication_id) as { entry_id: string; text: string; source_kinds: string }[]).map((e) => ({
        entry_id: e.entry_id, text: e.text, source_kinds: JSON.parse(e.source_kinds),
        evidence: this.#db.prepare('SELECT evidence_id AS id, title, text FROM knowledge_publication_evidence WHERE issuer = ? AND subject = ? AND publication_id = ? AND entry_id = ? ORDER BY position')
          .all(row.issuer, row.subject, row.publication_id, e.entry_id),
      }))
      : [];
    const removal = row.admin_blocked
      ? (this.#db.prepare('SELECT reason, removed_by, removed_at FROM knowledge_publication_removals WHERE issuer = ? AND subject = ? AND publication_id = ?')
        .get(row.issuer, row.subject, row.publication_id) as { reason: string; removed_by: string; removed_at: string } | undefined) ?? null
      : null;
    return { publication: key, removal, owner: owner?.user_name ?? null, revision: row.revision, state: row.admin_blocked ? 'removed' : content ? 'published' : 'withdrawn',
      title: content?.title ?? null, groups, updated_at: content?.updated_at ?? null, entries };
  }

  #byKey(key: string) {
    const row = this.#db.prepare('SELECT issuer, subject, publication_id, revision, admin_blocked FROM knowledge_publication_ledger WHERE public_key = ?').get(key) as
      { issuer: string; subject: string; publication_id: string; revision: number; admin_blocked: number } | undefined;
    if (!row) throw new ApiError(404, 'NOT_FOUND', 'No such publication.');
    return row;
  }

  /** Remove a publication with a reason; it stays blocked until an administrator resolves the removal. */
  adminRemove(admin: User, key: string, reason: unknown) {
    if (typeof reason !== 'string' || !reason.trim() || reason.length > 500) throw new ApiError(400, 'INVALID_REQUEST', 'A reason of up to 500 characters is required.');
    return this.#adminChange(admin, key, 'admin-remove', reason.trim());
  }

  adminResolve(admin: User, key: string) {
    return this.#adminChange(admin, key, 'admin-resolve', null);
  }

  #adminChange(admin: User, key: string, action: PublicationAction, reason: string | null) {
    const row = this.#byKey(key);
    const owner = { issuer: row.issuer, subject: row.subject, publicationId: row.publication_id };
    const result = commitPublicationRevision(this.#db, {
      ...owner, operationId: randomUUID(), expectedRevision: row.revision, action, requestEncoding: JSON.stringify([key, action]),
    }, () => {}, (revision) => {
      if (action === 'admin-remove') {
        removePublicationContent(this.#db, owner);
        this.#db.prepare(`INSERT INTO knowledge_publication_removals (issuer, subject, publication_id, revision, reason, removed_by, removed_at) VALUES (?, ?, ?, ?, ?, ?, ?)
          ON CONFLICT (issuer, subject, publication_id) DO UPDATE SET revision = excluded.revision, reason = excluded.reason, removed_by = excluded.removed_by, removed_at = excluded.removed_at`)
          .run(owner.issuer, owner.subject, owner.publicationId, revision, reason!, admin.userName, nowIso());
      }
    });
    this.#audit.write({ category: 'knowledge', action: `knowledge.publication.${action}`, outcome: 'ok', subject: admin.subject, userName: admin.userName, reason,
      details: { publication: key, revision: result.committedRevision } });
    return { publication: key, revision: result.committedRevision, state: action === 'admin-remove' ? 'removed' : 'withdrawn' };
  }

  /** Identifiers, revision, audience and outcome only: never titles, entry text or evidence. */
  #record(user: User, action: string, owner: { issuer: string; subject: string; publicationId: string }, revision: number, groups: string[] | null,
    replayed: boolean, extra: Record<string, unknown> = {}) {
    this.#audit.write({ category: 'knowledge', action, outcome: 'ok', subject: user.subject, userName: user.userName, reason: null, details: {
      publication: publicationKey(owner.issuer, owner.subject, owner.publicationId), publication_id: owner.publicationId, revision, replayed,
      ...(groups ? { groups } : {}), ...extra,
    } });
  }
}

const wrap =
  (fn: (req: Request) => unknown) =>
  async (req: Request, res: Response, next: NextFunction) => {
    try {
      res.json(await fn(req));
    } catch (err) {
      next(err);
    }
  };
const body = (req: Request) => (req.body && typeof req.body === 'object' && !Array.isArray(req.body) ? (req.body as Record<string, unknown>) : {});

/** Client side, mounted under /api/v1/knowledge for signed-in users. */
export function publicationRoutes(p: TeamPublications): Router {
  const r = Router();
  r.get('/capabilities', wrap(() => p.capabilities()));
  r.get('/publications', wrap((req) => p.mine(req.user!)));
  r.post('/publications/search', wrap((req) => p.search(req.user!, body(req))));
  r.get('/publications/shared/:key', wrap((req) => p.read(req.user!, String(req.params.key), req.query.offset, req.query.limit)));
  r.put('/publications/:id', wrap((req) => p.publish(req.user!, String(req.params.id), req.get(PUBLICATION_VERSION_HEADER), req.body)));
  r.delete('/publications/:id', wrap((req) => p.withdraw(req.user!, String(req.params.id), null, req.get(PUBLICATION_VERSION_HEADER), body(req))));
  r.delete('/publications/:id/entries/:entry', wrap((req) =>
    p.withdraw(req.user!, String(req.params.id), String(req.params.entry), req.get(PUBLICATION_VERSION_HEADER), body(req))));
  return r;
}

/** Admin side, platform-admins, mounted under /api/v1/admin/knowledge. */
export function publicationAdminRoutes(p: TeamPublications): Router {
  const r = Router();
  r.get('/publications', wrap(() => p.adminList()));
  r.get('/publications/:key', wrap((req) => p.adminRead(String(req.params.key))));
  r.delete('/publications/:key', wrap((req) => p.adminRemove(req.user!, String(req.params.key), body(req).reason)));
  r.post('/publications/:key/resolve', wrap((req) => p.adminResolve(req.user!, String(req.params.key))));
  return r;
}

