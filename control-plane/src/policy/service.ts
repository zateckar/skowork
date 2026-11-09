// control-plane/policy-service: Group definitions, policy documents with drafts and versions, the org guardrail,
// staged rollout, signed per-user bundles, policy-changed notification, simulation, diff and dispatch rules
// (design D7, D11-D14).
import { createHash } from 'node:crypto';
import { SignJWT, type JWK } from 'jose';
import type { Config } from '../config.ts';
import type { Db } from '../db.ts';
import { nowIso, tx } from '../db.ts';
import { KeyRing } from '../keys.ts';
import { ApiError } from '../http.ts';
import type { Audit } from '../identity/audit.ts';
import { EVERYONE, type User } from '../identity/identity.ts';
import type { PushHub, PushTarget } from '../fleet/push.ts';
import { type CompileError, type Compiled, type GroupDoc, compile, crossKeyErrors, entryPath } from './merge.ts';
import { type Flat, Registry, isPlainObject } from './schema.ts';
import { KEYS, SCHEMA_VERSION } from './keys.ts';
import { simulateToolDecision, type SimulatedCall } from './tool-decision.ts';

export const GUARDRAIL_SCOPE = 'guardrail';
export const groupScope = (name: string) => `group:${name}`;
const GROUP_NAME = /^[a-z0-9][a-z0-9_-]{0,63}$/;
const BUNDLE_TYP = 'workbench-policy+jwt';

export type Actor = Pick<User, 'subject' | 'userName' | 'adminRoles'>;

export interface Rollout {
  pilot_group?: string;
  percent?: number;
}

interface ScopeRow {
  scope: string;
  current_version: number | null;
  previous_version: number | null;
  candidate_version: number | null;
  rollout_kind: 'pilot' | 'percent' | null;
  rollout_value: string | null;
}

interface VersionRow {
  scope: string;
  version: number;
  priority: number;
  content: string;
  created_by: string;
  created_at: string;
  reason: string;
}

export interface DispatchRule {
  id: number;
  issuer_kind: 'user' | 'role' | 'group' | 'group-owner' | 'service-principal';
  issuer: string;
  target_kind: 'user' | 'group' | 'any';
  target: string;
}

export type DispatchIssuer =
  | { kind: 'user'; userName: string; adminRoles: string[]; groups: string[] }
  | { kind: 'service'; name: string };
export type DispatchTarget = { kind: 'user'; userName: string; groups: string[] } | { kind: 'group'; group: string } | { kind: 'everyone' };

/** Enabled MCP catalog entries for a set of Groups (control-plane/mcp-catalog); the stamp changes with any entry. */
export interface CatalogSource {
  entriesFor(groups: string[]): unknown[];
}

/** Rollout bucket: the first four bytes of the SHA-256 of the lower-case UPN, modulo 100 (design D12). */
export function rolloutBucket(userName: string): number {
  return createHash('sha256').update(userName.toLowerCase(), 'utf8').digest().readUInt32BE(0) % 100;
}

export class PolicyService {
  readonly registry = new Registry(SCHEMA_VERSION, KEYS);
  readonly #cfg: Config;
  readonly #db: Db;
  readonly #audit: Audit;
  readonly #push: PushHub;
  #catalog: CatalogSource = { entriesFor: () => [] };
  readonly #keys: KeyRing;

  constructor(cfg: Config, db: Db, audit: Audit, push: PushHub) {
    this.#cfg = cfg;
    this.#db = db;
    this.#audit = audit;
    this.#push = push;
    // Rotated-out keys stay published for the bundle validity plus one day (D12).
    this.#keys = new KeyRing(db, 'policy', () => cfg.policy.bundleValidityS + 86400);
  }

  setCatalogSource(c: CatalogSource): void {
    this.#catalog = c;
  }

  // ---- authorization ------------------------------------------------------------------------------------------------

  isPlatformAdmin(a: Actor): boolean {
    return a.adminRoles.includes('platform-admin');
  }

  owners(group: string): string[] {
    const row = this.#db.prepare('SELECT owners FROM policy_groups WHERE name = ?').get(group) as { owners: string } | undefined;
    return row ? (JSON.parse(row.owners) as string[]) : [];
  }

  /** A platform-admin manages every scope; a group-owner only the Groups whose owner list names them (D14). */
  canManage(a: Actor, scope: string): boolean {
    if (this.isPlatformAdmin(a)) return true;
    if (!scope.startsWith('group:') || !a.adminRoles.includes('group-owner')) return false;
    const me = a.userName.toLowerCase();
    return this.owners(scope.slice(6)).some((o) => o.toLowerCase() === me);
  }

  /** Throws (and audits the refusal) unless the actor may manage the scope. */
  assertManage(a: Actor, scope: string, action: string): void {
    if (this.canManage(a, scope)) return;
    this.#audit.write({
      category: 'policy', action, outcome: 'refused', subject: a.subject, userName: a.userName,
      reason: 'not an owner of this scope', details: { scope },
    });
    throw new ApiError(403, 'FORBIDDEN', 'You may not manage this policy.');
  }

  #record(a: Actor, action: string, details: Record<string, unknown>, reason?: string): void {
    this.#audit.write({ category: 'policy', action, outcome: 'ok', subject: a.subject, userName: a.userName, reason: reason ?? null, details });
  }

  // ---- Groups -------------------------------------------------------------------------------------------------------

  /** Every known Group (admin-console Every Known Group Is Listed): defined ones, those with a policy version or
   *  draft, those with direct members or role mappings (control-plane/identity, same database), and everyone. */
  listGroups() {
    const defined = new Map(
      (this.#db.prepare('SELECT name, description, owners FROM policy_groups').all() as { name: string; description: string; owners: string }[]).map((g) => [g.name, g]),
    );
    const names = new Set<string>([EVERYONE, ...defined.keys()]);
    for (const r of this.#db.prepare("SELECT DISTINCT substr(scope, 7) AS name FROM policy_versions WHERE scope LIKE 'group:%'").all() as { name: string }[]) names.add(r.name);
    for (const r of this.#db.prepare('SELECT DISTINCT workbench_group AS name FROM group_members UNION SELECT DISTINCT workbench_group FROM role_mappings').all() as { name: string }[]) names.add(r.name);
    return [...names].sort().map((name) => {
      const g = defined.get(name);
      const s = this.#scope(groupScope(name));
      const draft = this.#version(groupScope(name), 0);
      return {
        name, defined: Boolean(g), description: g?.description ?? '', owners: g ? (JSON.parse(g.owners) as string[]) : [],
        current_version: s?.current_version ?? null, candidate_version: s?.candidate_version ?? null,
        rollout: s?.rollout_kind ? { kind: s.rollout_kind, value: s.rollout_value } : null, has_draft: Boolean(draft),
      };
    });
  }

  /** Every role mapping (policy.llm.models) in a published or rolling-out version, for the LLM connection in-use check. */
  modelUses(): { scope: string; role: string; model: string }[] {
    const out: { scope: string; role: string; model: string }[] = [];
    for (const s of this.#db.prepare('SELECT * FROM policy_scopes').all() as unknown as ScopeRow[]) {
      for (const v of new Set([s.current_version, s.candidate_version])) {
        const row = v === null ? undefined : this.#version(s.scope, v);
        const models = row ? this.#flat(row).get('llm.models') : undefined;
        if (!isPlainObject(models)) continue;
        for (const [role, model] of Object.entries(models)) {
          if (typeof model === 'string' && !out.some((o) => o.scope === s.scope && o.role === role && o.model === model)) out.push({ scope: s.scope, role, model });
        }
      }
    }
    return out;
  }

  defineGroup(a: Actor, name: string, description: string, owners: string[]) {
    if (!this.isPlatformAdmin(a)) {
      this.#audit.write({ category: 'policy', action: 'policy.group.define', outcome: 'refused', subject: a.subject, userName: a.userName, details: { group: name } });
      throw new ApiError(403, 'FORBIDDEN', 'Only a platform-admin defines Groups.');
    }
    if (!GROUP_NAME.test(name)) throw new ApiError(400, 'INVALID_REQUEST', 'Group names use lowercase letters, digits, - and _.');
    const before = this.#db.prepare('SELECT description, owners FROM policy_groups WHERE name = ?').get(name) as Record<string, unknown> | undefined;
    this.#db
      .prepare(
        `INSERT INTO policy_groups (name, description, owners, created_by, created_at) VALUES (?, ?, ?, ?, ?)
         ON CONFLICT (name) DO UPDATE SET description = excluded.description, owners = excluded.owners`,
      )
      .run(name, description, JSON.stringify(owners), a.userName, nowIso());
    this.#record(a, 'policy.group.define', { group: name, before: before ? { ...before, owners: JSON.parse(String(before.owners)) } : null, after: { description, owners } });
    return { name, description, owners };
  }

  // ---- drafts and versions ------------------------------------------------------------------------------------------

  #scope(scope: string): ScopeRow | undefined {
    return this.#db.prepare('SELECT * FROM policy_scopes WHERE scope = ?').get(scope) as ScopeRow | undefined;
  }

  #version(scope: string, version: number): VersionRow | undefined {
    return this.#db.prepare('SELECT * FROM policy_versions WHERE scope = ? AND version = ?').get(scope, version) as VersionRow | undefined;
  }

  #flat(row: VersionRow): Flat {
    return this.registry.validate(JSON.parse(row.content), { guardrail: row.scope === GUARDRAIL_SCOPE }).flat;
  }

  /** Validates a document for a scope; throws 400 with every error, naming the fields. */
  validateDocument(scope: string, doc: unknown): Flat {
    const { flat, errors } = this.registry.validate(doc, { guardrail: scope === GUARDRAIL_SCOPE });
    if (errors.length) {
      throw new ApiError(400, 'POLICY_INVALID', `Invalid policy: ${errors.map((e) => `policy.${e.path}: ${e.message}`).join('; ')}`, { errors });
    }
    const cross = crossKeyErrors(flat);
    if (cross.length) throw new ApiError(400, 'POLICY_CONFLICT', cross.map((e) => e.message).join('; '), { errors: cross });
    return flat;
  }

  saveDraft(a: Actor, scope: string, doc: unknown, priority: number, reason: string) {
    this.assertManage(a, scope, 'policy.draft.save');
    if (!Number.isInteger(priority) || priority < 0 || priority > 1000) throw new ApiError(400, 'INVALID_REQUEST', 'priority must be an integer from 0 to 1000.');
    this.validateDocument(scope, doc);
    const before = this.#version(scope, 0);
    tx(this.#db, () => {
      if (scope.startsWith('group:')) {
        // A Group is defined implicitly by its first policy document (Group Definition And Ownership).
        this.#db
          .prepare('INSERT OR IGNORE INTO policy_groups (name, description, owners, created_by, created_at) VALUES (?, ?, ?, ?, ?)')
          .run(scope.slice(6), '', '[]', a.userName, nowIso());
      }
      this.#db
        .prepare(
          `INSERT INTO policy_versions (scope, version, priority, content, created_by, created_at, reason) VALUES (?, 0, ?, ?, ?, ?, ?)
           ON CONFLICT (scope, version) DO UPDATE SET priority = excluded.priority, content = excluded.content,
             created_by = excluded.created_by, created_at = excluded.created_at, reason = excluded.reason`,
        )
        .run(scope, scope === GUARDRAIL_SCOPE ? 0 : priority, JSON.stringify(doc), a.userName, nowIso(), reason);
    });
    this.#record(a, 'policy.draft.save', {
      scope, before: before ? { priority: before.priority, content: JSON.parse(before.content) } : null, after: { priority, content: doc },
    }, reason);
    return { scope, version: 0, priority };
  }

  deleteDraft(a: Actor, scope: string) {
    this.assertManage(a, scope, 'policy.draft.delete');
    const before = this.#version(scope, 0);
    if (!before) throw new ApiError(404, 'NOT_FOUND', 'There is no draft.');
    this.#db.prepare('DELETE FROM policy_versions WHERE scope = ? AND version = 0').run(scope);
    this.#record(a, 'policy.draft.delete', { scope, before: JSON.parse(before.content), after: null });
    return { deleted: true };
  }

  versions(scope: string) {
    const s = this.#scope(scope);
    const rows = this.#db
      .prepare('SELECT version, priority, created_by, created_at, reason FROM policy_versions WHERE scope = ? ORDER BY version')
      .all(scope) as Omit<VersionRow, 'content' | 'scope'>[];
    return {
      scope,
      current_version: s?.current_version ?? null,
      previous_version: s?.previous_version ?? null,
      candidate_version: s?.candidate_version ?? null,
      rollout: s?.rollout_kind ? { kind: s.rollout_kind, value: s.rollout_kind === 'percent' ? Number(s.rollout_value) : s.rollout_value } : null,
      versions: rows.map((r) => ({ ...r, draft: r.version === 0 })),
    };
  }

  /**
   * Rollout Adoption (design D11): active installations of the scope's users by the version they run. An installation
   * runs the version recorded with its user's last bundle when it reports that bundle's version.
   */
  adoption(scope: string, activeDays = 30) {
    const s = this.#scope(scope);
    const key = scope === GUARDRAIL_SCOPE ? GUARDRAIL_SCOPE : scope.slice(6);
    const since = new Date(Date.now() - activeDays * 86_400_000).toISOString();
    const rows = this.#db
      .prepare(
        `SELECT i.policy_version AS reported, u.version AS issued, u.scopes FROM installations i
         LEFT JOIN user_policy_versions u ON u.user_name = i.user_name COLLATE NOCASE
         WHERE i.revoked_at IS NULL AND i.last_seen >= ?
           AND (? = 'guardrail' OR EXISTS (SELECT 1 FROM json_each(i.groups) g WHERE g.value = ?))`,
      )
      .all(since, scope === GUARDRAIL_SCOPE ? 'guardrail' : 'group', key) as { reported: string; issued: number | null; scopes: string | null }[];
    const byVersion = new Map<number, number>();
    let notUpdated = 0;
    for (const r of rows) {
      const v = r.issued !== null && r.reported === String(r.issued) ? (JSON.parse(r.scopes ?? '{}') as Record<string, number>)[key] : undefined;
      if (v === undefined) notUpdated++;
      else byVersion.set(v, (byVersion.get(v) ?? 0) + 1);
    }
    return {
      scope,
      current_version: s?.current_version ?? null,
      candidate_version: s?.candidate_version ?? null,
      rollout: s?.rollout_kind ? { kind: s.rollout_kind, value: s.rollout_kind === 'percent' ? Number(s.rollout_value) : s.rollout_value, selected_users: this.#rolloutUsers(s, true).length } : null,
      installations: rows.length,
      by_version: [...byVersion].sort((a, b) => b[0] - a[0]).map(([version, installations]) => ({ version, installations })),
      not_updated: notUpdated,
    };
  }

  version(scope: string, ref: string | number) {
    const v = this.#resolveRef(scope, ref);
    const row = this.#version(scope, v);
    if (!row) throw new ApiError(404, 'NOT_FOUND', 'No such policy version.');
    return { scope, version: row.version, draft: row.version === 0, priority: row.priority, document: JSON.parse(row.content), created_by: row.created_by, created_at: row.created_at, reason: row.reason };
  }

  #resolveRef(scope: string, ref: string | number): number {
    if (ref === 'draft') return 0;
    if (ref === 'current' || ref === 'previous' || ref === 'candidate') {
      const s = this.#scope(scope);
      const v = s?.[`${ref}_version` as const];
      if (v === null || v === undefined) throw new ApiError(404, 'NOT_FOUND', `There is no ${ref} version.`);
      return v;
    }
    const n = Number(ref);
    if (!Number.isInteger(n) || n < 0) throw new ApiError(400, 'INVALID_REQUEST', 'version must be a number, draft, current, previous or candidate.');
    return n;
  }

  #nextVersion(scope: string): number {
    const row = this.#db.prepare('SELECT MAX(version) AS v FROM policy_versions WHERE scope = ?').get(scope) as { v: number | null };
    return (row.v ?? 0) + 1;
  }

  /** Publishes the draft as the next version, to everyone or as a staged rollout (Policy Versions And Drafts; D12). */
  publish(a: Actor, scope: string, opts: { reason: string; rollout?: Rollout }) {
    this.assertManage(a, scope, 'policy.publish');
    const draft = this.#version(scope, 0);
    if (!draft) throw new ApiError(409, 'NO_DRAFT', 'There is no draft to publish.');
    const s = this.#scope(scope);
    if (s?.candidate_version) throw new ApiError(409, 'ROLLOUT_ACTIVE', 'A rollout is active; promote or cancel it first.');
    const rollout = this.#checkRollout(scope, opts.rollout);
    this.#assertNoNewConflicts(scope, draft);

    const version = this.#nextVersion(scope);
    tx(this.#db, () => {
      this.#db
        .prepare('INSERT INTO policy_versions (scope, version, priority, content, created_by, created_at, reason) VALUES (?, ?, ?, ?, ?, ?, ?)')
        .run(scope, version, draft.priority, draft.content, a.userName, nowIso(), opts.reason);
      this.#db.prepare('DELETE FROM policy_versions WHERE scope = ? AND version = 0').run(scope);
      if (rollout) {
        this.#upsertScope(scope, { current_version: s?.current_version ?? null, previous_version: s?.previous_version ?? null, candidate_version: version, rollout_kind: rollout.kind, rollout_value: rollout.value });
      } else {
        this.#upsertScope(scope, { current_version: version, previous_version: s?.current_version ?? null, candidate_version: null, rollout_kind: null, rollout_value: null });
      }
    });
    this.#record(a, 'policy.publish', { scope, old_version: s?.current_version ?? null, new_version: version, rollout: rollout ?? null }, opts.reason);
    const recipients = rollout ? this.#notifyRolloutUsers(scope, true, 'rollout') : this.#notifyScope(scope, 'publish');
    return { scope, version, rollout: rollout ?? null, notified: recipients };
  }

  #checkRollout(scope: string, r?: Rollout): { kind: 'pilot' | 'percent'; value: string } | null {
    if (!r || (r.pilot_group === undefined && r.percent === undefined)) return null;
    if (scope === GUARDRAIL_SCOPE) throw new ApiError(400, 'INVALID_REQUEST', 'Guardrails take effect for everyone at once; they have no staged rollout.');
    if (r.pilot_group !== undefined && r.percent !== undefined) throw new ApiError(400, 'INVALID_REQUEST', 'Choose a pilot Group or a percentage, not both.');
    if (r.pilot_group !== undefined) {
      if (typeof r.pilot_group !== 'string' || !GROUP_NAME.test(r.pilot_group)) throw new ApiError(400, 'INVALID_REQUEST', 'pilot_group is not a valid Group name.');
      return { kind: 'pilot', value: r.pilot_group };
    }
    if (!Number.isInteger(r.percent) || r.percent! < 1 || r.percent! > 99) throw new ApiError(400, 'INVALID_REQUEST', 'percent must be an integer from 1 to 99.');
    return { kind: 'percent', value: String(r.percent) };
  }

  #upsertScope(scope: string, v: Omit<ScopeRow, 'scope'>): void {
    this.#db
      .prepare(
        `INSERT INTO policy_scopes (scope, current_version, previous_version, candidate_version, rollout_kind, rollout_value, updated_at)
         VALUES (?, ?, ?, ?, ?, ?, ?)
         ON CONFLICT (scope) DO UPDATE SET current_version = excluded.current_version, previous_version = excluded.previous_version,
           candidate_version = excluded.candidate_version, rollout_kind = excluded.rollout_kind, rollout_value = excluded.rollout_value,
           updated_at = excluded.updated_at`,
      )
      .run(scope, v.current_version, v.previous_version, v.candidate_version, v.rollout_kind, v.rollout_value, nowIso());
  }

  promote(a: Actor, scope: string, reason: string) {
    this.assertManage(a, scope, 'policy.rollout.promote');
    const s = this.#scope(scope);
    if (!s?.candidate_version) throw new ApiError(409, 'NO_ROLLOUT', 'No rollout is active.');
    // Users who were not selected get the candidate now; the selected ones already have it.
    const recipientsBefore = this.#rolloutUsers(s, false);
    this.#upsertScope(scope, { current_version: s.candidate_version, previous_version: s.current_version, candidate_version: null, rollout_kind: null, rollout_value: null });
    this.#record(a, 'policy.rollout.promote', { scope, old_version: s.current_version, new_version: s.candidate_version }, reason);
    return { scope, version: s.candidate_version, notified: this.#notifyUsers(recipientsBefore, scope, 'promote') };
  }

  cancelRollout(a: Actor, scope: string, reason: string) {
    this.assertManage(a, scope, 'policy.rollout.cancel');
    const s = this.#scope(scope);
    if (!s?.candidate_version) throw new ApiError(409, 'NO_ROLLOUT', 'No rollout is active.');
    const selected = this.#rolloutUsers(s, true);
    this.#upsertScope(scope, { current_version: s.current_version, previous_version: s.previous_version, candidate_version: null, rollout_kind: null, rollout_value: null });
    this.#record(a, 'policy.rollout.cancel', { scope, cancelled_version: s.candidate_version, version: s.current_version }, reason);
    return { scope, version: s.current_version, notified: this.#notifyUsers(selected, scope, 'rollout-cancelled') };
  }

  /** Publishes a new version whose content equals the previous version (Staged Rollout: Rollback; D12). */
  rollback(a: Actor, scope: string, reason: string) {
    this.assertManage(a, scope, 'policy.rollback');
    const s = this.#scope(scope);
    if (s?.candidate_version) throw new ApiError(409, 'ROLLOUT_ACTIVE', 'A rollout is active; promote or cancel it first.');
    if (!s?.previous_version) throw new ApiError(409, 'NO_PREVIOUS', 'There is no previous version to roll back to.');
    const prev = this.#version(scope, s.previous_version)!;
    this.#assertNoNewConflicts(scope, prev);
    const version = this.#nextVersion(scope);
    tx(this.#db, () => {
      this.#db
        .prepare('INSERT INTO policy_versions (scope, version, priority, content, created_by, created_at, reason) VALUES (?, ?, ?, ?, ?, ?, ?)')
        .run(scope, version, prev.priority, prev.content, a.userName, nowIso(), reason || `rollback to version ${prev.version}`);
      this.#upsertScope(scope, { current_version: version, previous_version: s.current_version, candidate_version: null, rollout_kind: null, rollout_value: null });
    });
    this.#record(a, 'policy.rollback', { scope, old_version: s.current_version, new_version: version, copied_version: prev.version }, reason);
    return { scope, version, copied_version: prev.version, notified: this.#notifyScope(scope, 'rollback') };
  }

  // ---- compilation --------------------------------------------------------------------------------------------------

  /** Whether a user compiles with the scope's candidate during a rollout. */
  #selected(s: ScopeRow, userName: string, groups: string[]): boolean {
    if (!s.candidate_version || !s.rollout_kind) return false;
    if (s.rollout_kind === 'pilot') return groups.includes(s.rollout_value ?? '');
    return rolloutBucket(userName) < Number(s.rollout_value);
  }

  /**
   * The documents in force for a user's Groups. `drafts` puts every draft in place of its published version;
   * `override` replaces one scope's document (publish checks, diffs).
   */
  #inputs(groups: string[], userName: string | null, opts: { drafts?: boolean; override?: { scope: string; row: VersionRow | null } } = {}) {
    const pick = (scope: string): VersionRow | null => {
      if (opts.override?.scope === scope) return opts.override.row;
      if (opts.drafts) {
        const d = this.#version(scope, 0);
        if (d) return d;
      }
      const s = this.#scope(scope);
      if (!s) return null;
      const v = userName !== null && this.#selected(s, userName, groups) ? s.candidate_version : s.current_version;
      return v ? (this.#version(scope, v) ?? null) : null;
    };
    const docs: GroupDoc[] = [];
    const versions: Record<string, number> = {};
    for (const g of groups) {
      const row = pick(groupScope(g));
      if (!row) continue;
      docs.push({ group: g, priority: row.priority, version: row.version, flat: this.#flat(row) });
      versions[g] = row.version;
    }
    const gr = pick(GUARDRAIL_SCOPE);
    if (gr) versions[GUARDRAIL_SCOPE] = gr.version;
    return { groups: docs, guardrail: gr ? { version: gr.version, flat: this.#flat(gr) } : null, versions };
  }

  compileFor(groups: string[], userName: string | null, opts: { drafts?: boolean; override?: { scope: string; row: VersionRow | null } } = {}): Compiled & { versions: Record<string, number> } {
    const inp = this.#inputs(groups, userName, opts);
    const c = compile(this.registry, { groups: inp.groups, guardrail: inp.guardrail, mcpServers: this.#catalog.entriesFor(groups) });
    return { ...c, versions: inp.versions };
  }

  /** Group combinations to check on publish: the Group alone and every set of Groups seen together on an installation (D11). */
  #combinations(scope: string): string[][] {
    const rows = this.#db.prepare('SELECT DISTINCT groups FROM installations WHERE revoked_at IS NULL').all() as { groups: string }[];
    const sets = new Map<string, string[]>();
    for (const r of rows) {
      const gs = (JSON.parse(r.groups) as string[]).slice().sort();
      if (scope === GUARDRAIL_SCOPE || gs.includes(scope.slice(6))) sets.set(gs.join(','), gs);
    }
    if (scope.startsWith('group:')) sets.set(scope.slice(6), [scope.slice(6)]);
    return [...sets.values()];
  }

  #assertNoNewConflicts(scope: string, row: VersionRow): void {
    const found: { groups: string[]; error: CompileError }[] = [];
    for (const gs of this.#combinations(scope)) {
      const before = new Set(this.compileFor(gs, null).errors.map((e) => e.message));
      for (const e of this.compileFor(gs, null, { override: { scope, row } }).errors) {
        if (!before.has(e.message)) found.push({ groups: gs, error: e });
      }
    }
    if (found.length) {
      const msg = found.map((f) => `${f.error.message} for the Groups ${f.groups.join(', ')}`).join('; ');
      throw new ApiError(409, 'POLICY_CONFLICT', `Publishing would break a cross-key rule: ${msg}`, {
        conflicts: found.map((f) => ({ keys: f.error.keys, groups: f.groups, message: f.error.message })),
      });
    }
  }

  // ---- bundles ------------------------------------------------------------------------------------------------------

  /** Public verification keys: current, next, and rotated-out keys for the bundle validity plus one day (D12). */
  publicKeys(): Promise<{ keys: JWK[] }> {
    return this.#keys.publicKeys();
  }

  async rotateKeys(a: Actor) {
    if (!this.isPlatformAdmin(a)) throw new ApiError(403, 'FORBIDDEN', 'Only a platform-admin rotates the policy signing key.');
    const r = await this.#keys.rotate();
    this.#record(a, 'policy.keys.rotate', r);
    return r;
  }

  /**
   * The signed Policy bundle of a user (Signed Bundles, Bundle Versioning And Expiry, Bundle Reuse And Compile Failure).
   */
  async bundleFor(user: Pick<User, 'subject' | 'userName' | 'groups'>): Promise<{ bundle: string; version: number; expires_at: string; stale: boolean }> {
    const prev =this.#db.prepare('SELECT * FROM user_policy_versions WHERE user_name = ? COLLATE NOCASE').get(user.userName) as
      | { version: number; content_hash: string; bundle: string; issued_at: string; expires_at: string }
      | undefined;
    const now = Date.now();
    const c = this.compileFor(user.groups, user.userName);
    if (c.errors.length) {
      this.#audit.write({
        category: 'policy', action: 'policy.compile-failed', outcome: 'error', subject: user.subject, userName: user.userName,
        reason: c.errors.map((e) => e.message).join('; '), details: { groups: user.groups, errors: c.errors },
      });
      if (prev && Date.parse(prev.expires_at) > now) return { bundle: prev.bundle, version: prev.version, expires_at: prev.expires_at, stale: true };
      throw new ApiError(409, 'POLICY_COMPILE_FAILED', `Your policy cannot be compiled: ${c.errors.map((e) => e.message).join('; ')}`, { errors: c.errors });
    }
    const hash = createHash('sha256')
      .update(JSON.stringify({ policy: c.policy, src: c.src, groups: user.groups, schema: this.registry.version, uid: user.subject }))
      .digest('hex');
    const validityMs = this.#cfg.policy.bundleValidityS * 1000;
    const scopes = JSON.stringify(c.versions);
    if (prev && prev.content_hash === hash && now < Date.parse(prev.issued_at) + validityMs / 2) {
      // Same content: a newly published version with identical content counts as adopted (Rollout Adoption).
      this.#db.prepare('UPDATE user_policy_versions SET scopes = ? WHERE user_name = ? COLLATE NOCASE').run(scopes, user.userName);
      return { bundle: prev.bundle, version: prev.version, expires_at: prev.expires_at, stale: false };
    }
    const version = (prev?.version ?? 0) + 1;
    const key = await this.#keys.current();
    const iat = Math.floor(now / 1000);
    const exp = iat + this.#cfg.policy.bundleValidityS;
    const bundle = await new SignJWT({ ver: version, uid: user.subject, groups: user.groups, src: c.src, schema: this.registry.version, policy: c.policy })
      .setProtectedHeader({ alg: 'ES256', kid: key.kid, typ: BUNDLE_TYP })
      .setSubject(user.userName)
      .setIssuedAt(iat)
      .setExpirationTime(exp)
      .sign(key.key);
    const issued = new Date(iat * 1000).toISOString();
    const expires = new Date(exp * 1000).toISOString();
    this.#db
      .prepare(
        `INSERT INTO user_policy_versions (user_name, version, content_hash, bundle, issued_at, expires_at, scopes) VALUES (?, ?, ?, ?, ?, ?, ?)
         ON CONFLICT (user_name) DO UPDATE SET version = excluded.version, content_hash = excluded.content_hash, bundle = excluded.bundle,
           issued_at = excluded.issued_at, expires_at = excluded.expires_at, scopes = excluded.scopes`,
      )
      .run(user.userName, version, hash, bundle, issued, expires, scopes);
    return { bundle, version, expires_at: expires, stale: false };
  }

  // ---- notification -------------------------------------------------------------------------------------------------

  #notify(target: PushTarget, payload: { reason: string; group?: string }): number {
    return this.#push.send('policy-changed', target, payload).recipients;
  }

  #notifyScope(scope: string, reason: string): number {
    if (scope === GUARDRAIL_SCOPE) return this.#notify({ everyone: true }, { reason });
    return this.#notify({ group: scope.slice(6) }, { reason, group: scope.slice(6) });
  }

  /** Users of the scope's Group on known installations, selected (or not) by the active rollout. */
  #rolloutUsers(s: ScopeRow, selected: boolean): string[] {
    const group = s.scope.slice(6);
    const rows = this.#db
      .prepare(`SELECT DISTINCT user_name, groups FROM installations i WHERE revoked_at IS NULL AND EXISTS (SELECT 1 FROM json_each(i.groups) g WHERE g.value = ?)`)
      .all(group) as { user_name: string; groups: string }[];
    const users = new Map<string, string>();
    for (const r of rows) {
      if (this.#selected(s, r.user_name, JSON.parse(r.groups) as string[]) === selected) users.set(r.user_name.toLowerCase(), r.user_name);
    }
    return [...users.values()];
  }

  #notifyRolloutUsers(scope: string, selected: boolean, reason: string): number {
    return this.#notifyUsers(this.#rolloutUsers(this.#scope(scope)!, selected), scope, reason);
  }

  #notifyUsers(users: string[], scope: string, reason: string): number {
    let n = 0;
    for (const u of users) n += this.#notify({ user: u }, { reason, group: scope.slice(6) });
    return n;
  }

  /** Role mappings decide everybody's Groups, so a change notifies every installation (Notification Scope). */
  mappingChanged(): number {
    return this.#notify({ everyone: true }, { reason: 'group-mapping' });
  }

  /** A direct assignment affects only the assigned user. */
  assignmentChanged(userName: string): number {
    return this.#notify({ user: userName }, { reason: 'group-assignment' });
  }

  /** An MCP catalog entry or assignment changed for these Groups (or for everyone). */
  catalogChanged(groups: string[] | 'everyone'): number {
    if (groups === 'everyone') return this.#notify({ everyone: true }, { reason: 'mcp-catalog' });
    return groups.reduce((n, g) => n + this.#notify({ group: g }, { reason: 'mcp-catalog', group: g }), 0);
  }

  // ---- simulation and diff ------------------------------------------------------------------------------------------

  /** The effective policy of a user or a set of Groups, with sources, conflicts and errors (Policy Simulation). */
  simulate(opts: { user?: string; groups?: string[]; drafts?: boolean; toolCall?: SimulatedCall }, groupsOfUser: (userName: string) => string[]) {
    let groups: string[];
    let userName: string | null = null;
    if (opts.user) {
      userName = opts.user;
      groups = groupsOfUser(opts.user);
    } else if (opts.groups?.length) {
      groups = [...new Set(opts.groups)].sort();
    } else {
      throw new ApiError(400, 'INVALID_REQUEST', 'Name a user or a set of Groups.');
    }
    const c = this.compileFor(groups, userName, { drafts: opts.drafts });
    const enforcement = userName
      ? (this.#db
          .prepare("SELECT network_enforcement AS level, COUNT(*) AS installations FROM installations WHERE user_name = ? COLLATE NOCASE AND revoked_at IS NULL GROUP BY network_enforcement")
          .all(userName) as { level: string; installations: number }[]).map((r) => ({ level: r.level || 'unknown', installations: r.installations }))
      : [];
    return {
      user: userName, groups, drafts: Boolean(opts.drafts), versions: c.versions, policy: c.policy, src: c.src,
      conflicts: c.conflicts, errors: c.errors, network_enforcement: enforcement, schema: this.registry.version,
      ...(opts.toolCall && (c.errors.length
        ? { tool_decision: null, tool_decision_error: 'Resolve policy errors before simulating a tool decision.' }
        : { tool_decision: simulateToolDecision(c.policy, c.src, opts.toolCall) })),
    };
  }

  /** Per-key differences between two versions of a scope, and optionally the change in a user's effective policy. */
  diff(scope: string, from: string, to: string, user?: { userName: string; groups: string[] }) {
    // "none" is the empty document, for a scope that has no version in force yet.
    const empty: VersionRow = { scope, version: 0, priority: 0, content: '{}', created_by: '', created_at: '', reason: '' };
    const a = from === 'none' ? empty : this.#version(scope, this.#resolveRef(scope, from));
    const b = to === 'none' ? empty : this.#version(scope, this.#resolveRef(scope, to));
    if (!a || !b) throw new ApiError(404, 'NOT_FOUND', 'No such policy version.');
    const out: Record<string, unknown> = {
      scope, from: a.version, to: b.version,
      priority: a.priority === b.priority ? undefined : { old: a.priority, new: b.priority },
      changes: diffFlat(this.#flat(a), this.#flat(b)),
    };
    if (user) {
      const before = this.compileFor(user.groups, user.userName, { override: { scope, row: a } });
      const after = this.compileFor(user.groups, user.userName, { override: { scope, row: b } });
      out.effective = { user: user.userName, groups: user.groups, changes: diffFlat(before.flat, after.flat), errors: after.errors };
    }
    return out;
  }

  // ---- dispatch rules -----------------------------------------------------------------------------------------------

  listDispatchRules(): DispatchRule[] {
    return this.#db.prepare('SELECT id, issuer_kind, issuer, target_kind, target FROM dispatch_rules ORDER BY id').all() as unknown as DispatchRule[];
  }

  addDispatchRule(a: Actor, r: Omit<DispatchRule, 'id'>) {
    if (!this.isPlatformAdmin(a)) throw new ApiError(403, 'FORBIDDEN', 'Only a platform-admin manages dispatch rules.');
    const issuerKinds = ['user', 'role', 'group', 'group-owner', 'service-principal'];
    const targetKinds = ['user', 'group', 'any'];
    if (!issuerKinds.includes(r.issuer_kind) || !targetKinds.includes(r.target_kind) || !r.issuer?.trim()) {
      throw new ApiError(400, 'INVALID_REQUEST', `issuer_kind (${issuerKinds.join(', ')}), issuer and target_kind (${targetKinds.join(', ')}) are required.`);
    }
    if (r.target_kind !== 'any' && !r.target?.trim()) throw new ApiError(400, 'INVALID_REQUEST', 'target is required unless target_kind is any.');
    const target = r.target_kind === 'any' ? '' : r.target.trim();
    const row = this.#db
      .prepare(
        `INSERT INTO dispatch_rules (issuer_kind, issuer, target_kind, target, created_by, created_at) VALUES (?, ?, ?, ?, ?, ?)
         ON CONFLICT DO UPDATE SET created_by = created_by RETURNING id`,
      )
      .get(r.issuer_kind, r.issuer.trim(), r.target_kind, target, a.userName, nowIso()) as { id: number };
    const rule = { id: row.id, issuer_kind: r.issuer_kind, issuer: r.issuer.trim(), target_kind: r.target_kind, target };
    this.#record(a, 'policy.dispatch-rule.add', { before: null, after: rule });
    return rule;
  }

  removeDispatchRule(a: Actor, id: number) {
    if (!this.isPlatformAdmin(a)) throw new ApiError(403, 'FORBIDDEN', 'Only a platform-admin manages dispatch rules.');
    const row = this.#db.prepare('DELETE FROM dispatch_rules WHERE id = ? RETURNING id, issuer_kind, issuer, target_kind, target').get(id);
    if (!row) throw new ApiError(404, 'NOT_FOUND', 'No such dispatch rule.');
    this.#record(a, 'policy.dispatch-rule.remove', { before: row, after: null });
    return { removed: true };
  }

  /** Dispatch Rule Matching: authorized only when some rule matches both issuer and target; admin roles alone never do. */
  authorizeDispatch(issuer: DispatchIssuer, target: DispatchTarget): { allowed: boolean; rule?: DispatchRule } {
    for (const r of this.listDispatchRules()) {
      if (issuerMatches(r, issuer, (g) => this.owners(g)) && targetMatches(r, target)) return { allowed: true, rule: r };
    }
    return { allowed: false };
  }
}

const eqi = (a: string, b: string) => a.toLowerCase() === b.toLowerCase();

function issuerMatches(r: DispatchRule, i: DispatchIssuer, owners: (group: string) => string[]): boolean {
  if (i.kind === 'service') return r.issuer_kind === 'service-principal' && r.issuer === i.name;
  switch (r.issuer_kind) {
    case 'user':
      return eqi(r.issuer, i.userName);
    case 'role':
      return i.adminRoles.includes(r.issuer);
    case 'group':
      return i.groups.includes(r.issuer);
    case 'group-owner':
      return i.adminRoles.includes('group-owner') && owners(r.issuer).some((o) => eqi(o, i.userName));
    default:
      return false;
  }
}

function targetMatches(r: DispatchRule, t: DispatchTarget): boolean {
  if (r.target_kind === 'any') return true;
  if (t.kind === 'user') return r.target_kind === 'user' ? eqi(r.target, t.userName) : t.groups.includes(r.target);
  if (t.kind === 'group') return r.target_kind === 'group' && r.target === t.group;
  return false;
}

/** Added, removed and changed keys; map-valued keys are compared entry by entry. */
export function diffFlat(a: Flat, b: Flat) {
  const expand = (f: Flat) => {
    const m = new Map<string, unknown>();
    for (const [k, v] of f) {
      if (isPlainObject(v)) for (const [e, x] of Object.entries(v)) m.set(entryPath(k, e), x);
      else m.set(k, v);
    }
    return m;
  };
  const x = expand(a);
  const y = expand(b);
  const added: { path: string; value: unknown }[] = [];
  const removed: { path: string; value: unknown }[] = [];
  const changed: { path: string; old: unknown; new: unknown }[] = [];
  for (const [k, v] of y) {
    if (!x.has(k)) added.push({ path: k, value: v });
    else if (JSON.stringify(x.get(k)) !== JSON.stringify(v)) changed.push({ path: k, old: x.get(k), new: v });
  }
  for (const [k, v] of x) if (!y.has(k)) removed.push({ path: k, value: v });
  const byPath = (p: { path: string }, q: { path: string }) => (p.path < q.path ? -1 : 1);
  return { added: added.sort(byPath), removed: removed.sort(byPath), changed: changed.sort(byPath) };
}
