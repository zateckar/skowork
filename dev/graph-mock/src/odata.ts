// The OData query subset the client emits (design D6): $filter with eq, ne, ge, gt, le, lt, and, or, not, contains,
// startswith and categories/any(); $select; $orderby on one or more fields; $top and $skip; $search as simple full text.
export class ODataError extends Error {}

type Value = string | number | boolean | null;
type Node =
  | { k: 'and' | 'or'; a: Node; b: Node }
  | { k: 'not'; a: Node }
  | { k: 'cmp'; op: string; path: string; v: Value }
  | { k: 'fn'; fn: 'contains' | 'startswith'; path: string; v: string }
  | { k: 'any'; path: string; op: string; v: Value };

function tokenize(s: string): string[] {
  const out: string[] = [];
  let i = 0;
  while (i < s.length) {
    const c = s[i]!;
    if (c === ' ') { i++; continue; }
    if (c === '(' || c === ')' || c === ',' || c === ':') { out.push(c); i++; continue; }
    if (c === "'") {
      let j = i + 1, v = "'";
      for (;;) {
        if (j >= s.length) throw new ODataError('unterminated string');
        if (s[j] === "'") {
          if (s[j + 1] === "'") { v += "'"; j += 2; continue; }
          break;
        }
        v += s[j++];
      }
      out.push(v + "'");
      i = j + 1;
      continue;
    }
    let j = i;
    // A ':' ends a word (lambda "c:c") except inside a date-time literal.
    while (j < s.length && !' (),'.includes(s[j]!) && (s[j] !== ':' || /^\d{4}-\d{2}-\d{2}T/.test(s.slice(i, j)))) j++;
    out.push(s.slice(i, j));
    i = j;
  }
  return out;
}

function literal(t: string | undefined): Value {
  if (t === undefined) throw new ODataError('missing value');
  if (t.startsWith("'")) return t.slice(1, -1);
  if (t === 'true') return true;
  if (t === 'false') return false;
  if (t === 'null') return null;
  if (/^-?\d+(\.\d+)?$/.test(t)) return Number(t);
  if (/^\d{4}-\d{2}-\d{2}(T[\d:.]+(Z|[+-]\d{2}:\d{2})?)?$/.test(t)) return t;
  throw new ODataError(`unsupported value '${t}'`);
}

const OPS = new Set(['eq', 'ne', 'gt', 'ge', 'lt', 'le']);

export function parseFilter(src: string): Node {
  const t = tokenize(src);
  let p = 0;
  const peek = () => t[p];
  const take = (want?: string) => {
    const v = t[p++];
    if (v === undefined || (want !== undefined && v.toLowerCase() !== want)) throw new ODataError(`expected '${want ?? 'token'}'`);
    return v;
  };
  const expr = (): Node => {
    let a = andExpr();
    while (peek()?.toLowerCase() === 'or') { take(); a = { k: 'or', a, b: andExpr() }; }
    return a;
  };
  const andExpr = (): Node => {
    let a = term();
    while (peek()?.toLowerCase() === 'and') { take(); a = { k: 'and', a, b: term() }; }
    return a;
  };
  const term = (): Node => {
    const tok = take();
    if (tok === '(') { const n = expr(); take(')'); return n; }
    if (tok.toLowerCase() === 'not') return { k: 'not', a: term() };
    const fn = tok.toLowerCase();
    if ((fn === 'contains' || fn === 'startswith') && peek() === '(') {
      take('(');
      const path = take();
      take(',');
      const v = literal(take());
      take(')');
      if (typeof v !== 'string') throw new ODataError(`${fn} needs a string`);
      return { k: 'fn', fn, path, v };
    }
    const any = /^(.+)\/any$/i.exec(tok);
    if (any && peek() === '(') {
      take('(');
      const v1 = take();
      take(':');
      const v2 = take();
      if (v1 !== v2) throw new ODataError('lambda variable mismatch');
      const op = take().toLowerCase();
      if (!OPS.has(op)) throw new ODataError(`unsupported operator '${op}'`);
      const v = literal(take());
      take(')');
      return { k: 'any', path: any[1]!, op, v };
    }
    const op = take().toLowerCase();
    if (!OPS.has(op)) throw new ODataError(`unsupported operator '${op}'`);
    return { k: 'cmp', op, path: tok, v: literal(take()) };
  };
  const n = expr();
  if (p !== t.length) throw new ODataError(`unexpected '${t[p]}'`);
  return n;
}

export function getPath(o: unknown, path: string): unknown {
  let v: unknown = o;
  for (const part of path.split('/')) {
    if (v === null || typeof v !== 'object') return undefined;
    v = (v as Record<string, unknown>)[part];
  }
  return v;
}

function compare(a: unknown, op: string, b: Value): boolean {
  let x: unknown = a, y: unknown = b;
  if (typeof a === 'string' && typeof b === 'string' && /^\d{4}-\d{2}-\d{2}/.test(a) && /^\d{4}-\d{2}-\d{2}/.test(b)) {
    const zone = (s: string) => (/(Z|[+-]\d{2}:\d{2})$/.test(s) ? s : s + 'Z');
    x = Date.parse(zone(a));
    y = Date.parse(zone(b));
  } else if (typeof a === 'string' && typeof b === 'string') {
    x = a.toLowerCase();
    y = b.toLowerCase();
  }
  switch (op) {
    case 'eq': return x === y || (x === undefined && y === null);
    case 'ne': return x !== y;
    case 'gt': return (x as number) > (y as number);
    case 'ge': return (x as number) >= (y as number);
    case 'lt': return (x as number) < (y as number);
    case 'le': return (x as number) <= (y as number);
  }
  return false;
}

export function evaluate(n: Node, o: unknown): boolean {
  switch (n.k) {
    case 'and': return evaluate(n.a, o) && evaluate(n.b, o);
    case 'or': return evaluate(n.a, o) || evaluate(n.b, o);
    case 'not': return !evaluate(n.a, o);
    case 'cmp': return compare(getPath(o, n.path), n.op, n.v);
    case 'fn': {
      const v = getPath(o, n.path);
      if (typeof v !== 'string') return false;
      return n.fn === 'contains' ? v.toLowerCase().includes(n.v.toLowerCase()) : v.toLowerCase().startsWith(n.v.toLowerCase());
    }
    case 'any': {
      const v = getPath(o, n.path);
      return Array.isArray(v) && v.some((x) => compare(x, n.op, n.v));
    }
  }
}

export function applyOrderBy<T>(items: T[], orderby: string | undefined): T[] {
  if (!orderby) return items;
  const keys = orderby.split(',').map((s) => {
    const [path, dir] = s.trim().split(/\s+/);
    if (!path) throw new ODataError('empty $orderby');
    return { path, desc: (dir ?? 'asc').toLowerCase() === 'desc' };
  });
  return [...items].sort((a, b) => {
    for (const k of keys) {
      const x = getPath(a, k.path) as string | number | undefined, y = getPath(b, k.path) as string | number | undefined;
      if (x === y) continue;
      if (x === undefined) return 1;
      if (y === undefined) return -1;
      return (x < y ? -1 : 1) * (k.desc ? -1 : 1);
    }
    return 0;
  });
}

export function applySelect(o: Record<string, unknown>, select: string | undefined): Record<string, unknown> {
  if (!select) return o;
  const keep = new Set(select.split(',').map((s) => s.trim()).filter(Boolean));
  const out: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(o)) if (k === 'id' || k.startsWith('@odata') || keep.has(k)) out[k] = v;
  return out;
}

/** $search="words": every word must occur in one of the fields. */
export function matchesSearch(fields: string[], search: string | undefined): boolean {
  if (!search) return true;
  const q = search.replace(/^"(.*)"$/, '$1').toLowerCase();
  const hay = fields.join(' \n ').toLowerCase();
  return q.split(/\s+/).filter(Boolean).every((w) => hay.includes(w));
}
