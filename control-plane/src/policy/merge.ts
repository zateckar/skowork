// Effective policy compilation (control-plane/policy-service Effective Policy Composition, Deny And Union Rules,
// Scalar Precedence, Sparse Effective Policy, Guardrail Strictness and Application, Cross-Key Validation; design D2,
// D9, D10, D11). The client never merges: it applies exactly what this produces.
import { type Decision, type Flat, type Registry, isPlainObject, unflatten } from './schema.ts';

export interface GroupDoc {
  group: string;
  priority: number;
  version: number;
  flat: Flat;
}

export interface CompileInput {
  guardrail?: { version: number; flat: Flat } | null;
  groups: GroupDoc[];
  /** Enabled MCP catalog entries assigned to the user's Groups (control-plane/mcp-catalog); not stored here. */
  mcpServers?: unknown[];
}

export interface Conflict {
  path: string;
  group: string;
  group_value: unknown;
  guardrail_value: unknown;
  effective: unknown;
}

export interface CompileError {
  code: 'CROSS_KEY';
  keys: string[];
  message: string;
}

export interface Compiled {
  policy: Record<string, unknown>;
  flat: Flat;
  /** Rule path → the Group, `guardrail` or `mcp-catalog` that produced it (design D1 `src`). */
  src: Record<string, string>;
  conflicts: Conflict[];
  errors: CompileError[];
}

export const GUARDRAIL = 'guardrail';
export const MCP_CATALOG = 'mcp-catalog';

/** Allow lists and the deny lists that prune them (design D9 list union). */
const PRUNED_BY: Record<string, string> = {
  'filesystem.read': 'filesystem.deny',
  'filesystem.write': 'filesystem.deny',
  'network.egress_allow': 'network.egress_deny',
};

/** The cross-key rule (Cross-Key Validation; design D11). */
export const CROSS_KEY_RULES: [lower: string, upper: string][] = [['scheduler.max_concurrent_runs', 'llm.max_concurrent_runs']];

const STRICTNESS: Record<Decision, number> = { allow: 0, approve: 1, deny: 2 };
export const stricter = (a: Decision, b: Decision): Decision => (STRICTNESS[a] >= STRICTNESS[b] ? a : b);

/** Path of one entry of a map-valued key, for `src` and conflicts. */
export const entryPath = (key: string, entry: string): string => `${key}[${JSON.stringify(entry)}]`;

/** Highest priority first; equal priorities by Group name in ordinal order, so the result is deterministic. */
export function byPrecedence(a: GroupDoc, b: GroupDoc): number {
  if (a.priority !== b.priority) return b.priority - a.priority;
  return a.group < b.group ? -1 : a.group > b.group ? 1 : 0;
}

export function compile(reg: Registry, input: CompileInput): Compiled {
  const groups = [...input.groups].sort(byPrecedence);
  const flat: Flat = new Map();
  const src: Record<string, string> = {};
  const conflicts: Conflict[] = [];
  // For conflicts: which Group gave a key (or a map entry) its pre-guardrail value.
  const origin = new Map<string, { group: string; value: unknown }>();

  const keysSet = new Set<string>();
  for (const g of groups) for (const k of g.flat.keys()) keysSet.add(k);

  for (const path of [...keysSet].sort()) {
    const def = reg.key(path);
    if (!def) continue;
    const setters = groups.filter((g) => g.flat.has(path));
    switch (def.merge) {
      case 'scalar':
      case 'whole-map': {
        const g = setters[0]!;
        flat.set(path, g.flat.get(path));
        src[path] = g.group;
        origin.set(path, { group: g.group, value: g.flat.get(path) });
        break;
      }
      case 'union': {
        const out: unknown[] = [];
        const seen = new Set<string>();
        for (const g of setters) {
          for (const item of g.flat.get(path) as unknown[]) {
            const id = typeof item === 'string' ? item.toLowerCase() : JSON.stringify(item);
            if (seen.has(id)) continue;
            seen.add(id);
            out.push(item);
            src[entryPath(path, String(item))] = g.group;
          }
        }
        flat.set(path, out);
        src[path] = setters.map((g) => g.group).join(',');
        break;
      }
      case 'decision-map': {
        // Per entry: deny if any Group denies it, otherwise the highest-priority Group that sets it.
        const out: Record<string, Decision> = {};
        const entries = new Set<string>();
        for (const g of setters) for (const e of Object.keys(g.flat.get(path) as object)) entries.add(e);
        for (const e of [...entries].sort()) {
          const with_ = setters.filter((g) => e in (g.flat.get(path) as Record<string, Decision>));
          const denier = with_.find((g) => (g.flat.get(path) as Record<string, Decision>)[e] === 'deny');
          const g = denier ?? with_[0]!;
          out[e] = (g.flat.get(path) as Record<string, Decision>)[e]!;
          src[entryPath(path, e)] = g.group;
          origin.set(entryPath(path, e), { group: g.group, value: out[e] });
        }
        flat.set(path, out);
        break;
      }
      case 'per-key-map': {
        const out: Record<string, unknown> = {};
        for (const g of setters) {
          for (const [e, v] of Object.entries(g.flat.get(path) as Record<string, unknown>)) {
            if (e in out) continue;
            out[e] = v;
            src[entryPath(path, e)] = g.group;
          }
        }
        flat.set(path, out);
        break;
      }
    }
  }

  // Guardrails: only ever stricter, and they apply to defaults that would break them (D10).
  const guard = input.guardrail?.flat ?? new Map<string, unknown>();
  for (const [path, gv] of [...guard.entries()].sort(([a], [b]) => (a < b ? -1 : 1))) {
    const def = reg.key(path);
    if (!def?.guardrail) continue;
    const set = flat.has(path);
    const current = set ? flat.get(path) : def.default;
    const conflict = (effective: unknown, at = path) => {
      const o = origin.get(at);
      if (o) conflicts.push({ path: at, group: o.group, group_value: o.value, guardrail_value: gv, effective });
    };
    const apply = (value: unknown) => {
      flat.set(path, value);
      src[path] = GUARDRAIL;
    };
    switch (def.guardrail) {
      case 'add': {
        const list = [...((set ? flat.get(path) : []) as unknown[])];
        const seen = new Set(list.map((x) => (typeof x === 'string' ? x.toLowerCase() : JSON.stringify(x))));
        for (const item of gv as unknown[]) {
          const id = typeof item === 'string' ? item.toLowerCase() : JSON.stringify(item);
          if (seen.has(id)) continue;
          seen.add(id);
          list.push(item);
          src[entryPath(path, String(item))] = GUARDRAIL;
        }
        flat.set(path, list);
        src[path] = set ? `${src[path]},${GUARDRAIL}` : GUARDRAIL;
        break;
      }
      case 'cap':
      case 'floor': {
        if (typeof current !== 'number') break;
        const broken = def.guardrail === 'cap' ? current > (gv as number) : current < (gv as number);
        if (broken) {
          if (set) conflict(gv);
          apply(gv);
        }
        break;
      }
      case 'max-level': {
        if (def.type.kind !== 'enum' || typeof current !== 'string') break;
        const order = def.type.values;
        if (order.indexOf(current) > order.indexOf(gv as string)) {
          if (set) conflict(gv);
          apply(gv);
        }
        break;
      }
      case 'restrict': {
        if (!Array.isArray(current)) break;
        const allowed = new Set((gv as unknown[]).map(String));
        const kept = current.filter((x) => allowed.has(String(x)));
        if (kept.length !== current.length) {
          if (set) conflict(kept);
          apply(kept);
        }
        break;
      }
      case 'force': {
        if (set && JSON.stringify(current) !== JSON.stringify(gv)) conflict(gv);
        apply(gv);
        break;
      }
      case 'force-entries': {
        const merged = { ...(isPlainObject(current) ? current : {}), ...(gv as Record<string, unknown>) };
        if (set && JSON.stringify(current) !== JSON.stringify(merged)) conflict(merged);
        flat.set(path, merged);
        for (const e of Object.keys(gv as object)) src[entryPath(path, e)] = GUARDRAIL;
        break;
      }
      case 'stricter': {
        const base: Record<string, Decision> = { ...((isPlainObject(current) ? current : {}) as Record<string, Decision>) };
        let changed = false;
        for (const [pattern, gd] of Object.entries(gv as Record<string, Decision>)) {
          for (const e of Object.keys(base)) {
            if (e !== pattern && !globMatch(pattern, e)) continue;
            const s = stricter(base[e]!, gd);
            if (s !== base[e]) {
              conflict(s, entryPath(path, e));
              base[e] = s;
              src[entryPath(path, e)] = GUARDRAIL;
              changed = true;
            }
          }
          if (!(pattern in base)) {
            base[pattern] = gd;
            src[entryPath(path, pattern)] = GUARDRAIL;
            changed = true;
          }
        }
        if (changed || set) flat.set(path, base);
        break;
      }
    }
  }

  // Allow lists lose every entry that a deny list of the same policy matches.
  for (const [allowKey, denyKey] of Object.entries(PRUNED_BY)) {
    const allow = flat.get(allowKey) as string[] | undefined;
    const deny = flat.get(denyKey) as string[] | undefined;
    if (!allow || !deny?.length) continue;
    const kept: string[] = [];
    for (const a of allow) {
      const by = deny.find((d) => globMatch(d, a));
      if (!by) {
        kept.push(a);
        continue;
      }
      const denySrc = src[entryPath(denyKey, by)];
      if (denySrc === GUARDRAIL) {
        conflicts.push({ path: entryPath(allowKey, a), group: src[entryPath(allowKey, a)] ?? '', group_value: a, guardrail_value: by, effective: 'denied' });
      }
      delete src[entryPath(allowKey, a)];
    }
    flat.set(allowKey, kept);
  }

  const errors: CompileError[] = [];
  for (const [lower, upper] of CROSS_KEY_RULES) {
    const lo = (flat.has(lower) ? flat.get(lower) : reg.defaultOf(lower)) as number | undefined;
    const up = (flat.has(upper) ? flat.get(upper) : reg.defaultOf(upper)) as number | undefined;
    if (typeof lo === 'number' && typeof up === 'number' && lo > up) {
      errors.push({ code: 'CROSS_KEY', keys: [lower, upper], message: `${lower} (${lo}) must not be greater than ${upper} (${up})` });
    }
  }

  if (input.mcpServers?.length) {
    flat.set('mcp.servers', input.mcpServers);
    src['mcp.servers'] = MCP_CATALOG;
  }

  const sorted = new Map([...flat.entries()].sort(([a], [b]) => (a < b ? -1 : 1)));
  return { policy: unflatten(sorted), flat: sorted, src, conflicts, errors };
}

/** Checks one document by itself against the cross-key rule (design D11: a document that sets both keys). */
export function crossKeyErrors(flat: Flat): CompileError[] {
  const out: CompileError[] = [];
  for (const [lower, upper] of CROSS_KEY_RULES) {
    const lo = flat.get(lower);
    const up = flat.get(upper);
    if (typeof lo === 'number' && typeof up === 'number' && lo > up) {
      out.push({ code: 'CROSS_KEY', keys: [lower, upper], message: `${lower} (${lo}) must not be greater than ${upper} (${up})` });
    }
  }
  return out;
}

/**
 * Case-insensitive glob: `**` matches across path segments, `*` within one segment (separators `\` and `/`), and `?`
 * one character. Used for allow-list pruning and guardrail decision patterns, the same rules as the client matcher.
 */
export function globMatch(pattern: string, text: string): boolean {
  let re = '';
  for (let i = 0; i < pattern.length; i++) {
    const c = pattern[i]!;
    if (c === '*') {
      if (pattern[i + 1] === '*') {
        re += '.*';
        i++;
      } else re += '[^\\\\/]*';
    } else if (c === '?') re += '[^\\\\/]';
    else re += c.replace(/[.+^${}()|[\]\\]/g, '\\$&');
  }
  return new RegExp(`^${re}$`, 'i').test(text);
}
