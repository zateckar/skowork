// The file diff a reviewer sees against the previous version of a Skill (control-plane/admin-console Skill Review Queue).
import type { ZipEntry } from './zip.ts';

export interface FileDiff {
  path: string;
  status: 'added' | 'removed' | 'changed' | 'unchanged';
  /** Unified-style lines (" ", "+", "-") for text files; absent for binary or very large files. */
  lines?: string[];
  note?: string;
}

const MAX_LINES = 2000;
const CONTEXT = 3;

const isText = (b: Buffer) => !b.subarray(0, 8000).includes(0);

/** Line diff by longest common subsequence, with three lines of context around changes. */
export function diffLines(a: string[], b: string[]): string[] {
  const n = a.length;
  const m = b.length;
  const lcs: Uint32Array[] = Array.from({ length: n + 1 }, () => new Uint32Array(m + 1));
  for (let i = n - 1; i >= 0; i--) for (let j = m - 1; j >= 0; j--) lcs[i]![j] = a[i] === b[j] ? lcs[i + 1]![j + 1]! + 1 : Math.max(lcs[i + 1]![j]!, lcs[i]![j + 1]!);
  const all: string[] = [];
  let i = 0;
  let j = 0;
  while (i < n || j < m) {
    if (i < n && j < m && a[i] === b[j]) (all.push(` ${a[i]}`), i++, j++);
    else if (i < n && (j === m || lcs[i + 1]![j]! >= lcs[i]![j + 1]!)) all.push(`-${a[i++]}`);
    else all.push(`+${b[j++]}`);
  }
  const keep = all.map(() => false);
  all.forEach((l, k) => {
    if (l[0] === ' ') return;
    for (let x = Math.max(0, k - CONTEXT); x <= Math.min(all.length - 1, k + CONTEXT); x++) keep[x] = true;
  });
  const out: string[] = [];
  let skipped = false;
  all.forEach((l, k) => {
    if (keep[k]) {
      out.push(l);
      skipped = false;
    } else if (!skipped) {
      out.push('@@');
      skipped = true;
    }
  });
  return out;
}

export function diffFiles(before: ZipEntry[], after: ZipEntry[]): FileDiff[] {
  const prev = new Map(before.map((f) => [f.path, f.data]));
  const next = new Map(after.map((f) => [f.path, f.data]));
  const paths = [...new Set([...prev.keys(), ...next.keys()])].sort();
  return paths.map((path) => {
    const a = prev.get(path);
    const b = next.get(path);
    const status: FileDiff['status'] = !a ? 'added' : !b ? 'removed' : a.equals(b) ? 'unchanged' : 'changed';
    if (status === 'unchanged') return { path, status };
    if ((a && !isText(a)) || (b && !isText(b))) return { path, status, note: 'binary file' };
    const la = a ? a.toString('utf8').split(/\r\n?|\n/) : [];
    const lb = b ? b.toString('utf8').split(/\r\n?|\n/) : [];
    if (la.length > MAX_LINES || lb.length > MAX_LINES) return { path, status, note: `more than ${MAX_LINES} lines` };
    return { path, status, lines: diffLines(la, lb) };
  });
}
