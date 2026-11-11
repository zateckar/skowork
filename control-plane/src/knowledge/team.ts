// control-plane/team-knowledge (add-knowledge-sharing D2): knowledge items users share with their Groups. The
// server chunks the text, embeds it through the gateway's `embed` role on the owner's behalf and stores chunks with a
// full-text index and int8 vectors. Search fuses full-text and vector ranks (reciprocal rank fusion) over the items
// of the caller's Groups only.
import { Router, type NextFunction, type Request, type Response } from 'express';
import { createHash } from 'node:crypto';
import type { Config } from '../config.ts';
import type { Db } from '../db.ts';
import { nowIso, tx } from '../db.ts';
import { ApiError, str } from '../http.ts';
import type { Audit } from '../identity/audit.ts';
import { EVERYONE, type User } from '../identity/identity.ts';

const CHUNK_CHARS = 1024; // the client's chunk size and overlap (client/knowledge-graph, design D3)
const OVERLAP_CHARS = 128;
const EMBED_BATCH = 64;
const TOP_K = 50;
const RRF_K = 60;
const ITEM_ID = /^[A-Za-z0-9._:-]{1,200}$/;

export type Embedder = (user: User, input: string[]) => Promise<{ model: string; vectors: number[][] } | null>;
export type SharingPolicy = (user: User) => string[];
export type CurrentGroups = (user: User) => string[];
export type PublicationPassages = (user: User, q: string, limit: number) => Promise<{
  publication: string; owner: string; groups: string[]; revision: number; title: string; updated_at: string;
  entry_id: string; chunk: number; text: string; score: number; evidence: { id: string; title: string; text: string }[];
}[]>;

interface ItemRow {
  id: number;
  owner: string;
  item_id: string;
  title: string;
  kind: 'files' | 'notes';
  content_hash: string;
  server_content_hash: string;
  size: number;
  embed_model: string | null;
  created_at: string;
  updated_at: string;
  owner_issuer: string | null; // NULL: a legacy share, keyed only by its historical username
  owner_subject: string | null;
}

/** The caller's subject-bound shares and their legacy shares under the current username. */
const OWNED = '((owner_issuer = ? AND owner_subject = ?) OR (owner_subject IS NULL AND owner = ?))';
const ownedArgs = (user: User) => [user.issuer, user.subject, user.userName];

/** Paragraph-aware windows of about CHUNK_CHARS with an overlap, like the client's document chunker. */
export function chunkText(text: string): string[] {
  const out: string[] = [];
  const paras = text.replace(/\r\n/g, '\n').split(/\n{2,}/).map((p) => p.trim()).filter(Boolean);
  let cur = '';
  const flush = () => {
    if (cur.trim()) out.push(cur.trim());
    cur = cur.length > OVERLAP_CHARS ? cur.slice(cur.length - OVERLAP_CHARS).replace(/^\S*\s/, '') : '';
  };
  for (const p of paras) {
    if (p.length > CHUNK_CHARS) {
      if (cur) flush();
      for (let i = 0; i < p.length; i += CHUNK_CHARS - OVERLAP_CHARS) out.push(p.slice(i, i + CHUNK_CHARS));
      cur = '';
      continue;
    }
    if (cur && cur.length + 2 + p.length > CHUNK_CHARS) flush();
    cur = cur ? `${cur}\n\n${p}` : p;
  }
  if (cur.trim()) out.push(cur.trim());
  return out;
}

/** Unit-length int8 quantisation; the dot product of two such vectors orders like their cosine similarity. */
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

/** Words of the query as an FTS5 OR-query of quoted terms, so no query syntax reaches FTS5. */
function ftsQuery(q: string): string | null {
  const words = [...new Set(q.toLowerCase().match(/[\p{L}\p{N}]{2,}/gu) ?? [])].slice(0, 32);
  return words.length ? words.map((w) => `"${w}"`).join(' OR ') : null;
}

export class TeamKnowledge {
  readonly #cfg: Config;
  readonly #db: Db;
  readonly #audit: Audit;
  readonly #embed: Embedder;
  readonly #sharing: SharingPolicy;
  readonly #currentGroups: CurrentGroups;
  #publicationPassages: PublicationPassages | null = null;

  constructor(cfg: Config, db: Db, audit: Audit, embed: Embedder, sharing: SharingPolicy, currentGroups: CurrentGroups) {
    this.#cfg = cfg;
    this.#db = db;
    this.#audit = audit;
    this.#embed = embed;
    this.#sharing = sharing;
    this.#currentGroups = currentGroups;
  }

  /**
   * The Groups the user may share with: the Groups policy.knowledge.sharing.groups names (or `*`: all of the user's
   * Groups but everyone) that the user is a member of. Stored as the owner's snapshot, which decides which of their
   * items are returned until their next request.
   */
  permitted(user: User): string[] {
    const policy = this.#sharing(user);
    const member = new Set(user.groups ?? []);
    const out = new Set<string>();
    for (const g of policy) {
      if (g === '*') for (const m of member) if (m !== EVERYONE) out.add(m);
      if (member.has(g)) out.add(g);
    }
    const permitted = [...out].sort();
    this.#db
      .prepare(
        `INSERT INTO team_owners (user_name, groups, permitted, updated_at, issuer, subject) VALUES (?, ?, ?, ?, ?, ?)
         ON CONFLICT (user_name) DO UPDATE SET groups = excluded.groups, permitted = excluded.permitted, updated_at = excluded.updated_at,
           issuer = excluded.issuer, subject = excluded.subject`,
      )
      .run(user.userName, JSON.stringify([...member].sort()), JSON.stringify(permitted), nowIso(), user.issuer, user.subject);
    // A subject-bound share follows its owner's current username. A share whose id the new name already holds keeps the
    // former name, and stays hidden while that name's snapshot belongs to another subject.
    this.#db
      .prepare('UPDATE OR IGNORE team_items SET owner = ? WHERE owner_issuer = ? AND owner_subject = ? AND owner <> ?')
      .run(user.userName, user.issuer, user.subject, user.userName);
    return permitted;
  }

  #record(user: User, action: string, details: Record<string, unknown>, reason?: string) {
    this.#audit.write({ category: 'knowledge', action, outcome: 'ok', subject: user.subject, userName: user.userName, reason: reason ?? null, details });
  }

  #currentUser(user: User): User {
    if (user.tokenExpiresAt + this.#cfg.identity.clockSkewS <= Date.now() / 1000)
      throw new ApiError(401, 'UNAUTHORIZED', 'Your session expired while the request was being prepared.');
    const revoked = this.#db.prepare("SELECT 1 FROM deny_list WHERE kind = 'user' AND lower(value) = lower(?)").get(user.userName);
    if (revoked) throw new ApiError(403, 'FORBIDDEN', 'This user is no longer authorized.');
    return { ...user, groups: this.#currentGroups(user) };
  }

  /** The caller's subject-bound share with this id. */
  #owned(user: User, itemId: string): ItemRow | undefined {
    return this.#db.prepare('SELECT * FROM team_items WHERE owner_issuer = ? AND owner_subject = ? AND item_id = ?')
      .get(user.issuer, user.subject, itemId) as ItemRow | undefined;
  }

  /** A legacy share with this id under the caller's current username: not proof that it was the caller's. */
  #legacy(user: User, itemId: string): ItemRow | undefined {
    return this.#db.prepare('SELECT * FROM team_items WHERE owner_subject IS NULL AND owner = ? AND item_id = ?')
      .get(user.userName, itemId) as ItemRow | undefined;
  }

  #usage(user: User, except: number[]): number {
    const rows = this.#db.prepare(`SELECT id, size FROM team_items WHERE ${OWNED}`).all(...ownedArgs(user)) as { id: number; size: number }[];
    return rows.filter((r) => !except.includes(Number(r.id))).reduce((s, r) => s + Number(r.size), 0);
  }

  #state(row: ItemRow | undefined): string {
    return row ? JSON.stringify([row, this.#groupsOf(row.id)]) : '';
  }

  #groupsOf(id: number): string[] {
    return (this.#db.prepare('SELECT grp FROM team_item_groups WHERE item = ? ORDER BY grp').all(id) as { grp: string }[]).map((r) => r.grp);
  }

  /** Shared Item Publication: a new item or a replacement of the owner's item with the same id. */
  async publish(user: User, itemId: string, body: Record<string, unknown>) {
    if (!ITEM_ID.test(itemId)) throw new ApiError(400, 'INVALID_REQUEST', 'The item id must be 1-200 letters, digits, dots, colons, underscores or dashes.');
    const { title, kind, text, content_hash: hash, groups } = body;
    if (typeof title !== 'string' || !title.trim() || title.length > 300) throw new ApiError(400, 'INVALID_REQUEST', 'title must be 1-300 characters.');
    if (kind !== 'files' && kind !== 'notes') throw new ApiError(400, 'INVALID_REQUEST', 'kind must be files or notes.');
    if (typeof text !== 'string') throw new ApiError(400, 'INVALID_REQUEST', 'text is required.');
    if (typeof hash !== 'string' || !hash || hash.length > 128) throw new ApiError(400, 'INVALID_REQUEST', 'content_hash is required.');
    if (!Array.isArray(groups) || !groups.length || !groups.every((g) => typeof g === 'string')) throw new ApiError(400, 'INVALID_REQUEST', 'groups must name at least one Group.');
    const size = Buffer.byteLength(text, 'utf8');
    const limits = this.#cfg.teamKnowledge;
    if (size > limits.maxItemBytes) throw new ApiError(413, 'TOO_LARGE', `The text is larger than ${Math.round(limits.maxItemBytes / 1024 / 1024)} MB.`);
    const permitted = this.permitted(user);
    const refused = (groups as string[]).filter((g) => !permitted.includes(g));
    if (refused.length) {
      throw new ApiError(403, 'GROUP_NOT_PERMITTED', `You cannot share with ${refused.join(', ')}: you are not a member, or policy.knowledge.sharing.groups does not permit it.`, { groups: refused });
    }
    const ownerSnapshot = this.#db.prepare('SELECT groups, permitted FROM team_owners WHERE user_name = ?').get(user.userName) as { groups: string; permitted: string };
    const before = this.#owned(user, itemId);
    // A legacy share of this username and id is replaced, not adopted: its content is removed with the new write.
    const legacy = before ? undefined : this.#legacy(user, itemId);
    const beforeState = this.#state(before);
    const legacyState = this.#state(legacy);
    const taken = () => !before && !legacy && this.#db.prepare('SELECT 1 FROM team_items WHERE owner = ? AND item_id = ?').get(user.userName, itemId);
    if (taken()) throw new ApiError(409, 'CONFLICT', 'Another account still holds a shared item with this id under your username.');
    const replaced = [before?.id, legacy?.id].filter((x): x is number => x !== undefined).map(Number);
    if (this.#usage(user, replaced) + size > limits.quotaBytes) throw new ApiError(413, 'QUOTA_EXCEEDED', 'Your shared knowledge is at its size limit. Unshare items first.');

    const target = [...new Set(groups as string[])].sort();
    const serverHash = createHash('sha256').update(text, 'utf8').digest('hex');
    const sameText = before?.server_content_hash === serverHash;
    let chunks: string[] = [];
    let embedded: { model: string; vectors: number[][] } | null = null;
    if (!sameText) {
      chunks = chunkText(text);
      // Embedded on the owner's behalf, in batches; without the embed role the item is found by full text only.
      const vectors: number[][] = [];
      let model: string | null = null;
      for (let i = 0; i < chunks.length; i += EMBED_BATCH) {
        const r = await this.#embed(user, chunks.slice(i, i + EMBED_BATCH));
        if (!r || (model && r.model !== model)) {
          model = null;
          break;
        }
        model = r.model;
        vectors.push(...r.vectors);
      }
      if (model && vectors.length === chunks.length) embedded = { model, vectors };
    }
    // Refresh the stored owner snapshot before the write transaction, so a rejected
    // request cannot roll back a newly observed loss of Group or policy access.
    const freshUser = this.#currentUser(user);
    const currentOwner = this.#db.prepare('SELECT groups, permitted FROM team_owners WHERE user_name = ?').get(user.userName) as { groups: string; permitted: string } | undefined;
    if (!currentOwner || currentOwner.groups !== ownerSnapshot.groups || currentOwner.permitted !== ownerSnapshot.permitted)
      throw new ApiError(409, 'CONFLICT', 'Your sharing permissions changed while the item was being prepared. Refresh and try again.');
    const stillPermitted = this.permitted(freshUser);
    if (target.some((g) => !stillPermitted.includes(g)))
      throw new ApiError(403, 'GROUP_NOT_PERMITTED', 'Sharing permission changed while the item was being prepared.');
    const now = nowIso();
    const id = tx(this.#db, () => {
      // Nothing yields between the refresh and this transaction. Check the item and
      // quota again because other requests may have committed while embedding.
      if (this.#state(this.#owned(user, itemId)) !== beforeState || this.#state(before ? undefined : this.#legacy(user, itemId)) !== legacyState || taken())
        throw new ApiError(409, 'CONFLICT', 'The shared item changed while it was being prepared. Refresh and try again.');
      if (this.#usage(user, replaced) + size > limits.quotaBytes)
        throw new ApiError(413, 'QUOTA_EXCEEDED', 'Your shared knowledge is at its size limit. Unshare items first.');
      if (legacy) this.#deleteRow(legacy.id);
      let rowId: number;
      if (before) {
        rowId = before.id;
        this.#db.prepare('UPDATE team_items SET title = ?, kind = ?, content_hash = ?, server_content_hash = ?, size = ?, updated_at = ? WHERE id = ?')
          .run(title.trim(), kind, hash, serverHash, size, now, rowId);
      } else {
        rowId = Number(
          this.#db
            .prepare(`INSERT INTO team_items (owner, item_id, title, kind, content_hash, server_content_hash, size, embed_model, created_at, updated_at, owner_issuer, owner_subject)
                      VALUES (?, ?, ?, ?, ?, ?, ?, NULL, ?, ?, ?, ?)`)
            .run(user.userName, itemId, title.trim(), kind, hash, serverHash, size, now, now, user.issuer, user.subject).lastInsertRowid,
        );
      }
      if (!sameText) {
        this.#db.prepare('DELETE FROM team_chunks WHERE item = ?').run(rowId);
        const ins = this.#db.prepare('INSERT INTO team_chunks (item, seq, text, vector) VALUES (?, ?, ?, ?)');
        chunks.forEach((c, i) => ins.run(rowId, i, c, embedded ? quantise(embedded.vectors[i]!) : null));
        this.#db.prepare('UPDATE team_items SET embed_model = ? WHERE id = ?').run(embedded?.model ?? null, rowId);
      }
      this.#db.prepare('DELETE FROM team_item_groups WHERE item = ?').run(rowId);
      for (const g of target) this.#db.prepare('INSERT INTO team_item_groups (item, grp) VALUES (?, ?)').run(rowId, g);
      return rowId;
    });
    this.#record(user, 'knowledge.publish', { item_id: itemId, title: title.trim(), kind, size, groups: target, replaced: Boolean(before), replaced_legacy: Boolean(legacy), text_changed: !sameText });
    return this.#view(this.#db.prepare('SELECT * FROM team_items WHERE id = ?').get(id) as unknown as ItemRow);
  }

  #view(r: ItemRow) {
    const chunks = (this.#db.prepare('SELECT COUNT(*) AS n FROM team_chunks WHERE item = ?').get(r.id) as { n: number }).n;
    return {
      item_id: r.item_id, title: r.title, kind: r.kind, owner: r.owner, size: r.size, content_hash: r.content_hash, groups: this.#groupsOf(r.id),
      chunks: Number(chunks), embedded: r.embed_model !== null, created_at: r.created_at, updated_at: r.updated_at,
      ownership: r.owner_subject === null ? 'legacy' : 'subject',
    };
  }

  /** The caller's own shared items: what the client shows as shared (an item an admin removed is missing here). */
  mine(user: User) {
    const permitted = this.permitted(user);
    const items = this.#db.prepare(`SELECT * FROM team_items WHERE ${OWNED} ORDER BY updated_at DESC`).all(...ownedArgs(user)) as unknown as ItemRow[];
    return { permitted_groups: permitted, items: items.map((r) => this.#view(r)) };
  }

  /**
   * Withdrawal of the caller's share. Not found is `ABSENT` only when no share could be the caller's: none of this subject
   * and no legacy share with this id under any username, since a legacy share may predate a username change.
   */
  unshare(user: User, itemId: string) {
    const r = this.#owned(user, itemId) ?? this.#legacy(user, itemId);
    if (!r) {
      const legacyElsewhere = this.#db.prepare('SELECT 1 FROM team_items WHERE owner_subject IS NULL AND item_id = ?').get(itemId);
      throw new ApiError(404, legacyElsewhere ? 'NOT_FOUND' : 'ABSENT', 'You have no shared item with this id.');
    }
    const groups = this.#groupsOf(r.id);
    this.#delete(r.id);
    const ownership = r.owner_subject === null ? 'legacy' : 'subject';
    this.#record(user, 'knowledge.unshare', { item_id: itemId, title: r.title, groups, ownership });
    return { unshared: itemId, ownership };
  }

  #deleteRow(id: number) {
    this.#db.prepare('DELETE FROM team_chunks WHERE item = ?').run(id);
    this.#db.prepare('DELETE FROM team_items WHERE id = ?').run(id);
  }

  #delete(id: number) {
    tx(this.#db, () => this.#deleteRow(id));
  }

  /**
   * Items visible to the caller through a Group: shared with a Group the caller is in, which the owner was a member
   * of and permitted to share with at their last request, from an owner who is not revoked.
   */
  #visible(user: User): Map<number, string[]> {
    const mine = new Set(user.groups ?? []);
    const rows = this.#db
      .prepare(
        `SELECT g.item, g.grp, o.groups AS og, o.permitted AS op FROM team_item_groups g
         JOIN team_items i ON i.id = g.item
         JOIN team_owners o ON o.user_name = i.owner
           AND (i.owner_subject IS NULL OR (o.issuer = i.owner_issuer AND o.subject = i.owner_subject))
         WHERE NOT EXISTS (SELECT 1 FROM deny_list d WHERE d.kind = 'user' AND lower(d.value) = lower(i.owner))`,
      )
      .all() as { item: number; grp: string; og: string; op: string }[];
    const out = new Map<number, string[]>();
    for (const r of rows) {
      if (!mine.has(r.grp)) continue;
      if (!(JSON.parse(r.og) as string[]).includes(r.grp) || !(JSON.parse(r.op) as string[]).includes(r.grp)) continue;
      out.set(r.item, [...(out.get(r.item) ?? []), r.grp]);
    }
    return out;
  }

  /** Publication passages for the same caller and query, merged into item search for clients without publication support. */
  setPublicationPassages(fn: PublicationPassages) {
    this.#publicationPassages = fn;
  }

  /**
   * Team Knowledge Search: passages of items shared with the caller's Groups, full text and vectors fused, together
   * with authorized publication passages in the item passage shape, each naming its publication and revision.
   */
  async search(user: User, body: Record<string, unknown>) {
    const q = typeof body.query === 'string' ? body.query.trim().slice(0, 2000) : '';
    if (!q) throw new ApiError(400, 'INVALID_REQUEST', 'query is required.');
    const limit = Math.min(50, Math.max(1, Number.isInteger(body.limit) ? (body.limit as number) : 10));
    const items = await this.#itemPassages(user, q, limit);
    if (!this.#publicationPassages) return { passages: items };
    const published = (await this.#publicationPassages(user, q, limit)).map((p) => ({
      item_id: `publication:${p.publication}:${p.entry_id}`, title: p.title, owner: p.owner, groups: p.groups, kind: 'notes' as const,
      updated_at: p.updated_at, chunk: p.chunk, text: p.text, score: p.score,
      publication: { key: p.publication, revision: p.revision, entry_id: p.entry_id, evidence: p.evidence },
    }));
    return { passages: [...items, ...published].sort((a, b) => b.score - a.score).slice(0, limit) };
  }

  async #itemPassages(user: User, q: string, limit: number) {
    this.permitted(user); // refresh the caller's own snapshot
    const visible = this.#visible(user);
    if (!visible.size) return [];
    const ids = [...visible.keys()];
    const inList = ids.join(',');
    const ranks: Map<number, number>[] = [];

    const fts = ftsQuery(q);
    if (fts) {
      const rows = this.#db
        .prepare(`SELECT c.id FROM team_chunks_fts f JOIN team_chunks c ON c.id = f.rowid WHERE team_chunks_fts MATCH ? AND c.item IN (${inList}) ORDER BY bm25(team_chunks_fts) LIMIT ${TOP_K}`)
        .all(fts) as { id: number }[];
      ranks.push(new Map(rows.map((r, i) => [r.id, i])));
    }
    const models = (this.#db.prepare(`SELECT DISTINCT embed_model AS m FROM team_items WHERE id IN (${inList}) AND embed_model IS NOT NULL`).all() as { m: string }[]).map((r) => r.m);
    if (models.length) {
      const e = await this.#embed(user, [q]);
      if (e && models.includes(e.model)) {
        const qv = quantise(e.vectors[0]!);
        const rows = this.#db
          .prepare(`SELECT c.id, c.vector FROM team_chunks c JOIN team_items i ON i.id = c.item WHERE c.item IN (${inList}) AND i.embed_model = ? AND c.vector IS NOT NULL`)
          .all(e.model) as { id: number; vector: Uint8Array }[];
        const scored = rows
          .filter((r) => r.vector.length === qv.length)
          .map((r) => ({ id: r.id, s: dot(qv, Buffer.from(r.vector)) }))
          .sort((a, b) => b.s - a.s)
          .slice(0, TOP_K);
        ranks.push(new Map(scored.map((r, i) => [r.id, i])));
      }
    }
    const fused = new Map<number, number>();
    for (const m of ranks) for (const [id, rank] of m) fused.set(id, (fused.get(id) ?? 0) + 1 / (RRF_K + rank + 1));
    // The embedding call above yields to other requests. Recheck stored visibility before
    // materializing results: an item may have been withdrawn, or its owner revoked, meanwhile.
    const stillVisible = this.#visible(this.#currentUser(user));
    const top = [...fused.entries()].sort((a, b) => b[1] - a[1]);
    const passages: { item_id: string; title: string; owner: string; groups: string[]; kind: 'files' | 'notes'; updated_at: string; chunk: number; text: string; score: number }[] = [];
    for (const [chunkId, score] of top) {
      const c = this.#db
        .prepare('SELECT c.text, c.seq, i.* FROM team_chunks c JOIN team_items i ON i.id = c.item WHERE c.id = ?')
        .get(chunkId) as (ItemRow & { text: string; seq: number }) | undefined;
      if (!c || !visible.has(c.id) || !stillVisible.has(c.id)) continue;
      passages.push({
        item_id: c.item_id, title: c.title, owner: c.owner, groups: stillVisible.get(c.id)!, kind: c.kind, updated_at: c.updated_at,
        chunk: c.seq, text: c.text, score: Number(score.toFixed(5)),
      });
      if (passages.length >= limit) break;
    }
    return passages;
  }

  // ---- administration ------------------------------------------------------------------------------------------

  adminList(group: string | undefined) {
    const rows = (group
      ? this.#db.prepare('SELECT i.* FROM team_items i JOIN team_item_groups g ON g.item = i.id WHERE g.grp = ? ORDER BY i.updated_at DESC').all(group)
      : this.#db.prepare('SELECT * FROM team_items ORDER BY updated_at DESC').all()) as unknown as ItemRow[];
    const groups = (this.#db.prepare('SELECT grp, COUNT(*) AS n, SUM(i.size) AS size FROM team_item_groups g JOIN team_items i ON i.id = g.item GROUP BY grp ORDER BY grp').all() as {
      grp: string; n: number; size: number;
    }[]).map((g) => ({ group: g.grp, items: Number(g.n), size: Number(g.size) }));
    return { groups, items: rows.map((r) => ({ id: r.id, ...this.#view(r) })) };
  }

  adminRemove(admin: User, id: number, reason: unknown) {
    if (typeof reason !== 'string' || !reason.trim()) throw new ApiError(400, 'INVALID_REQUEST', 'A reason is required.');
    const r = this.#db.prepare('SELECT * FROM team_items WHERE id = ?').get(id) as ItemRow | undefined;
    if (!r) throw new ApiError(404, 'NOT_FOUND', 'No such shared item.');
    const groups = this.#groupsOf(r.id);
    this.#delete(r.id);
    this.#record(admin, 'knowledge.remove', { owner: r.owner, item_id: r.item_id, title: r.title, groups }, reason.trim());
    return { removed: id };
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
const body = (req: Request) => (req.body && typeof req.body === 'object' ? (req.body as Record<string, unknown>) : {});

/** Client side, for signed-in users: /api/v1/knowledge. */
export function teamKnowledgeRoutes(t: TeamKnowledge): Router {
  const r = Router();
  r.get('/items', wrap((req) => t.mine(req.user!)));
  r.put('/items/:id', wrap((req) => t.publish(req.user!, String(req.params.id), body(req))));
  r.delete('/items/:id', wrap((req) => t.unshare(req.user!, String(req.params.id))));
  r.post('/search', wrap((req) => t.search(req.user!, body(req))));
  return r;
}

/** Admin side, platform-admins: /api/v1/admin/knowledge. */
export function teamKnowledgeAdminRoutes(t: TeamKnowledge): Router {
  const r = Router();
  r.get('/items', wrap((req) => t.adminList(str(req.query.group))));
  r.delete('/items/:id', wrap((req) => t.adminRemove(req.user!, Number(req.params.id), body(req).reason)));
  return r;
}
