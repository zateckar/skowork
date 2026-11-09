// The policy schema registry (control-plane/policy-service Policy Schema Ownership; design D9, D10). Every policy key is
// declared once in keys.ts. Validation, the merge, the guardrail modes and the generated JSON Schema all read it.

export type Decision = 'allow' | 'approve' | 'deny';
export const DECISIONS: Decision[] = ['allow', 'approve', 'deny'];
export const RISK_CLASSES = ['read', 'write-local', 'external', 'destructive'] as const;

export type KeyType =
  | { kind: 'boolean' }
  | { kind: 'integer'; min?: number; max?: number }
  | { kind: 'number'; min?: number; max?: number }
  | { kind: 'string'; pattern?: string; maxLength?: number }
  | { kind: 'enum'; values: string[] }
  | { kind: 'list'; item: KeyType; maxItems?: number }
  | { kind: 'decision-map'; keys?: string[] }
  | { kind: 'map'; value: KeyType; keys?: string[] };

export type MergeClass = 'scalar' | 'union' | 'decision-map' | 'whole-map' | 'per-key-map';
export type GuardrailMode = 'add' | 'cap' | 'floor' | 'max-level' | 'restrict' | 'force' | 'force-entries' | 'stricter';

export interface KeyDef {
  /** Dotted path below `policy.`, for example `tools.shell.mode`. */
  path: string;
  /** The capability whose spec defines the key. */
  owner: string;
  /** Administrator-facing explanation of what this setting controls. */
  description: string;
  type: KeyType;
  /** The default stated by the owning spec. Used only for the cross-key rule and guardrail comparisons (D9). */
  default?: unknown;
  merge: MergeClass;
  /** How a guardrail applies to the key; a key without a mode cannot be set in a guardrail (Guardrail Application). */
  guardrail?: GuardrailMode;
}

export interface ValidationError {
  path: string;
  message: string;
}

/** A policy document flattened to its leaf keys. */
export type Flat = Map<string, unknown>;

// Tool-id patterns: tool ids, `*` wildcards and MCP ids such as `mcp:jira-corp/*`.
const TOOL_PATTERN = /^[A-Za-z0-9_.:*\/-]{1,128}$/;

export class Registry {
  readonly version: number;
  readonly keys: ReadonlyMap<string, KeyDef>;
  readonly #prefixes = new Set<string>();

  constructor(version: number, defs: KeyDef[]) {
    this.version = version;
    const keys = new Map<string, KeyDef>();
    for (const d of defs) {
      if (typeof d.owner !== 'string' || !d.owner.trim() || typeof d.description !== 'string' || !d.description.trim())
        throw new Error(`policy key ${d.path} is missing its owner or description`);
      if (keys.has(d.path)) throw new Error(`policy key ${d.path} is declared twice`);
      keys.set(d.path, d);
    }
    for (const p of keys.keys()) {
      const parts = p.split('.');
      for (let i = 1; i < parts.length; i++) this.#prefixes.add(parts.slice(0, i).join('.'));
    }
    for (const p of keys.keys()) {
      if (this.#prefixes.has(p)) throw new Error(`policy key ${p} is both a key and a prefix of another key`);
    }
    this.keys = keys;
  }

  key(path: string): KeyDef | undefined {
    return this.keys.get(path);
  }

  defaultOf(path: string): unknown {
    return this.keys.get(path)?.default;
  }

  /**
   * Validates a nested policy document and flattens it to leaf keys. A guardrail document may only set keys that
   * have a guardrail mode.
   */
  validate(doc: unknown, opts: { guardrail?: boolean } = {}): { flat: Flat; errors: ValidationError[] } {
    const flat: Flat = new Map();
    const errors: ValidationError[] = [];
    if (!isPlainObject(doc)) {
      errors.push({ path: '', message: 'the policy document must be a JSON object' });
      return { flat, errors };
    }
    const walk = (obj: Record<string, unknown>, prefix: string) => {
      for (const [k, v] of Object.entries(obj)) {
        const path = prefix ? `${prefix}.${k}` : k;
        const def = this.keys.get(path);
        if (def) {
          const msg = checkValue(def.type, v);
          if (msg) errors.push({ path, message: msg });
          else if (opts.guardrail && !def.guardrail) errors.push({ path, message: 'this key has no meaning in a guardrail' });
          else flat.set(path, v);
        } else if (path === 'mcp.servers') {
          errors.push({ path, message: 'MCP servers come from the MCP catalog and cannot be set in a policy document' });
        } else if (this.#prefixes.has(path)) {
          if (isPlainObject(v)) walk(v, path);
          else errors.push({ path, message: 'expected an object' });
        } else {
          errors.push({ path, message: 'unknown policy key' });
        }
      }
    };
    walk(doc, '');
    return { flat, errors };
  }

  /** JSON Schema (draft 2020-12) of a policy document, generated from the registry. */
  jsonSchema(opts: { guardrail?: boolean } = {}): Record<string, unknown> {
    const root: Record<string, unknown> = { type: 'object', additionalProperties: false, properties: {} };
    for (const def of [...this.keys.values()].sort((a, b) => a.path.localeCompare(b.path))) {
      if (opts.guardrail && !def.guardrail) continue;
      const parts = def.path.split('.');
      let node = root;
      for (const part of parts.slice(0, -1)) {
        const props = node.properties as Record<string, Record<string, unknown>>;
        props[part] ??= { type: 'object', additionalProperties: false, properties: {} };
        node = props[part]!;
      }
      const leaf: Record<string, unknown> = { ...typeSchema(def.type), 'x-owner': def.owner, 'x-merge': def.merge };
      if (def.default !== undefined) leaf.default = def.default;
      if (def.guardrail) leaf['x-guardrail'] = def.guardrail;
      (node.properties as Record<string, unknown>)[parts.at(-1)!] = leaf;
    }
    return {
      $schema: 'https://json-schema.org/draft/2020-12/schema',
      $id: `urn:workbench:policy:${opts.guardrail ? 'guardrail' : 'document'}:v${this.version}`,
      title: opts.guardrail ? 'Workbench org guardrail document' : 'Workbench Group policy document',
      ...root,
    };
  }
}

export function isPlainObject(v: unknown): v is Record<string, unknown> {
  return typeof v === 'object' && v !== null && !Array.isArray(v);
}

/** Returns a reason when the value does not match the type. */
export function checkValue(t: KeyType, v: unknown): string | null {
  switch (t.kind) {
    case 'boolean':
      return typeof v === 'boolean' ? null : 'expected true or false';
    case 'integer':
    case 'number': {
      if (typeof v !== 'number' || !Number.isFinite(v)) return `expected a${t.kind === 'integer' ? 'n integer' : ' number'}`;
      if (t.kind === 'integer' && !Number.isInteger(v)) return 'expected an integer';
      if (t.min !== undefined && v < t.min) return `must be at least ${t.min}`;
      if (t.max !== undefined && v > t.max) return `must be at most ${t.max}`;
      return null;
    }
    case 'string': {
      if (typeof v !== 'string') return 'expected a string';
      if (t.maxLength !== undefined && v.length > t.maxLength) return `must be at most ${t.maxLength} characters`;
      if (t.pattern && !new RegExp(t.pattern).test(v)) return 'has an invalid format';
      return null;
    }
    case 'enum':
      return typeof v === 'string' && t.values.includes(v) ? null : `must be one of: ${t.values.join(', ')}`;
    case 'list': {
      if (!Array.isArray(v)) return 'expected a list';
      if (t.maxItems !== undefined && v.length > t.maxItems) return `must have at most ${t.maxItems} entries`;
      for (const [i, item] of v.entries()) {
        const m = checkValue(t.item, item);
        if (m) return `entry ${i}: ${m}`;
      }
      return null;
    }
    case 'decision-map': {
      if (!isPlainObject(v)) return 'expected a map of tool patterns to allow, approve or deny';
      for (const [k, d] of Object.entries(v)) {
        if (t.keys ? !t.keys.includes(k) : !TOOL_PATTERN.test(k)) return `invalid entry name "${k}"`;
        if (!DECISIONS.includes(d as Decision)) return `entry "${k}" must be allow, approve or deny`;
      }
      return null;
    }
    case 'map': {
      if (!isPlainObject(v)) return 'expected a map';
      for (const [k, x] of Object.entries(v)) {
        if (t.keys && !t.keys.includes(k)) return `invalid entry name "${k}"`;
        const m = checkValue(t.value, x);
        if (m) return `entry "${k}": ${m}`;
      }
      return null;
    }
  }
}

function typeSchema(t: KeyType): Record<string, unknown> {
  switch (t.kind) {
    case 'boolean':
      return { type: 'boolean' };
    case 'integer':
    case 'number':
      return { type: t.kind, ...(t.min !== undefined && { minimum: t.min }), ...(t.max !== undefined && { maximum: t.max }) };
    case 'string':
      return { type: 'string', ...(t.pattern && { pattern: t.pattern }), ...(t.maxLength !== undefined && { maxLength: t.maxLength }) };
    case 'enum':
      return { type: 'string', enum: t.values };
    case 'list':
      return { type: 'array', items: typeSchema(t.item), ...(t.maxItems !== undefined && { maxItems: t.maxItems }) };
    case 'decision-map':
      return t.keys
        ? { type: 'object', additionalProperties: false, properties: Object.fromEntries(t.keys.map((k) => [k, { enum: DECISIONS }])) }
        : { type: 'object', propertyNames: { pattern: TOOL_PATTERN.source }, additionalProperties: { enum: DECISIONS } };
    case 'map':
      return t.keys
        ? { type: 'object', additionalProperties: false, properties: Object.fromEntries(t.keys.map((k) => [k, typeSchema(t.value)])) }
        : { type: 'object', additionalProperties: typeSchema(t.value) };
  }
}

/** Builds the nested document of a flat key map. */
export function unflatten(flat: Iterable<[string, unknown]>): Record<string, unknown> {
  const root: Record<string, unknown> = {};
  for (const [path, v] of flat) {
    const parts = path.split('.');
    let node = root;
    for (const p of parts.slice(0, -1)) {
      if (!isPlainObject(node[p])) node[p] = {};
      node = node[p] as Record<string, unknown>;
    }
    node[parts.at(-1)!] = v;
  }
  return root;
}
