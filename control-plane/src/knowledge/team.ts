// control-plane/team-knowledge (add-knowledge-sharing D2): knowledge items users share with their Groups. The
// server chunks the text, embeds it through the gateway's `embed` role on the owner's behalf and stores chunks with a
// full-text index and int8 vectors. Search fuses full-text and vector ranks (reciprocal rank fusion) over the items
// of the caller's Groups only.
import { Router, type NextFunction, type Request, type Response } from 'express';
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

interface ItemRow {
  id: number;
  owner: string;
  item_id: string;
  title: string;
  kind: 'files' | 'notes';
  content_hash: string;
  size: number;
  embed_model: string | null;
  created_at: string;
  updated_at: string;
}

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

  constructor(cfg: Config, db: Db, audit: Audit, embed: Embedder, sharing: SharingPolicy) {
    this.#cfg = cfg;
    this.#db = db;
    this.#audit = audit;
    this.#embed = embed;
    this.#sharing = sharing;
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
        `INSERT INTO team_owners (user_name, groups, permitted, updated_at) VALUES (?, ?, ?, ?)
         ON CONFLICT (user_name) DO UPDATE SET groups = excluded.groups, permitted = excluded.permitted, updated_at = excluded.updated_at`,
      )
      .run(user.userName, JSON.stringify([...member].sort()), JSON.stringify(permitted), nowIso());
    return permitted;
  }

  #record(user: User, action: string, details: Record<string, unknown>, reason?: string) {
    this.#audit.write({ category: 'knowledge', action, outcome: 'ok', subject: user.subject, userName: user.userName, reason: reason ?? null, details });
  }

  #item(owner: string, itemId: string): ItemRow | undefined {
    return this.#db.prepare('SELECT * FROM team_items WHERE owner = ? AND item_id = ?').get(owner, itemId) as ItemRow | undefined;
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
    const before = this.#item(user.userName, itemId);
    const others = (this.#db.prepare('SELECT COALESCE(SUM(size), 0) AS s FROM team_items WHERE owner = ? AND item_id != ?').get(user.userName, itemId) as { s: number }).s;
    if (Number(others) + size > limits.quotaBytes) throw new ApiError(413, 'QUOTA_EXCEEDED', 'Your shared knowledge is at its size limit. Unshare items first.');

    const target = [...new Set(groups as string[])].sort();
    const sameText = before?.content_hash === hash;
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
    const now = nowIso();
    const id = tx(this.#db, () => {
      let rowId: number;
      if (before) {
        rowId = before.id;
        this.#db.prepare('UPDATE team_items SET title = ?, kind = ?, content_hash = ?, size = ?, updated_at = ? WHERE id = ?').run(title.trim(), kind, hash, size, now, rowId);
      } else {
        rowId = Number(
          this.#db
            .prepare('INSERT INTO team_items (owner, item_id, title, kind, content_hash, size, embed_model, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, NULL, ?, ?)')
            .run(user.userName, itemId, title.trim(), kind, hash, size, now, now).lastInsertRowid,
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
    this.#record(user, 'knowledge.publish', { item_id: itemId, title: title.trim(), kind, size, groups: target, replaced: Boolean(before), text_changed: !sameText });
    return this.#view(this.#db.prepare('SELECT * FROM team_items WHERE id = ?').get(id) as unknown as ItemRow);
  }

  #view(r: ItemRow) {
    const chunks = (this.#db.prepare('SELECT COUNT(*) AS n FROM team_chunks WHERE item = ?').get(r.id) as { n: number }).n;
    return {
      item_id: r.item_id, title: r.title, kind: r.kind, owner: r.owner, size: r.size, content_hash: r.content_hash, groups: this.#groupsOf(r.id),
      chunks: Number(chunks), embedded: r.embed_model !== null, created_at: r.created_at, updated_at: r.updated_at,
    };
  }

  /** The caller's own shared items: what the client shows as shared (an item an admin removed is missing here). */
  mine(user: User) {
    const permitted = this.permitted(user);
    const items = this.#db.prepare('SELECT * FROM team_items WHERE owner = ? ORDER BY updated_at DESC').all(user.userName) as unknown as ItemRow[];
    return { permitted_groups: permitted, items: items.map((r) => this.#view(r)) };
  }

  unshare(user: User, itemId: string) {
    const r = this.#item(user.userName, itemId);
    if (!r) throw new ApiError(404, 'NOT_FOUND', 'You have no shared item with this id.');
    const groups = this.#groupsOf(r.id);
    this.#delete(r.id);
    this.#record(user, 'knowledge.unshare', { item_id: itemId, title: r.title, groups });
    return { unshared: itemId };
  }

  #delete(id: number) {
    tx(this.#db, () => {
      this.#db.prepare('DELETE FROM team_chunks WHERE item = ?').run(id);
      this.#db.prepare('DELETE FROM team_items WHERE id = ?').run(id);
    });
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

  /** Team Knowledge Search: passages of items shared with the caller's Groups, full text and vectors fused. */
  async search(user: User, body: Record<string, unknown>) {
    const q = typeof body.query === 'string' ? body.query.trim().slice(0, 2000) : '';
    if (!q) throw new ApiError(400, 'INVALID_REQUEST', 'query is required.');
    const limit = Math.min(50, Math.max(1, Number.isInteger(body.limit) ? (body.limit as number) : 10));
    this.permitted(user); // refresh the caller's own snapshot
    const visible = this.#visible(user);
    if (!visible.size) return { passages: [] };
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
    const top = [...fused.entries()].sort((a, b) => b[1] - a[1]).slice(0, limit);
    const passages = top.map(([chunkId, score]) => {
      const c = this.#db
        .prepare('SELECT c.text, c.seq, i.* FROM team_chunks c JOIN team_items i ON i.id = c.item WHERE c.id = ?')
        .get(chunkId) as unknown as ItemRow & { text: string; seq: number };
      return {
        item_id: c.item_id, title: c.title, owner: c.owner, groups: visible.get(c.id) ?? [], kind: c.kind, updated_at: c.updated_at,
        chunk: c.seq, text: c.text, score: Number(score.toFixed(5)),
      };
    });
    return { passages };
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
