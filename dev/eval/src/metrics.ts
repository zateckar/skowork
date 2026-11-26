// Metrics of add-evaluation-export design D6.
import type { Turn } from './labels.ts';

export function precisionRecall(pred: boolean[], gold: boolean[]): { precision: number; recall: number } {
  let tp = 0;
  let fp = 0;
  let fn = 0;
  for (let i = 0; i < Math.min(pred.length, gold.length); i++) {
    if (pred[i] && gold[i]) tp++;
    else if (pred[i] && !gold[i]) fp++;
    else if (!pred[i] && gold[i]) fn++;
  }
  return { precision: tp + fp ? tp / (tp + fp) : 1, recall: tp + fn ? tp / (tp + fn) : 1 };
}

export function words(text: string): string[] {
  return text
    .toLowerCase()
    .replace(/[^\p{L}\p{N}\s'-]/gu, ' ')
    .split(/\s+/)
    .filter(Boolean);
}

function editDistance(a: string[], b: string[]): number {
  const prev = new Array<number>(b.length + 1);
  for (let j = 0; j <= b.length; j++) prev[j] = j;
  for (let i = 1; i <= a.length; i++) {
    let diag = prev[0]!;
    prev[0] = i;
    for (let j = 1; j <= b.length; j++) {
      const up = prev[j]!;
      prev[j] = Math.min(prev[j]! + 1, prev[j - 1]! + 1, diag + (a[i - 1] === b[j - 1] ? 0 : 1));
      diag = up;
    }
  }
  return prev[b.length]!;
}

// Word error rate: word-level edit distance over the reference length (lower case, punctuation removed).
export function wer(ref: string, hyp: string): number {
  const r = words(ref);
  if (r.length === 0) return words(hyp).length ? 1 : 0;
  return editDistance(r, words(hyp)) / r.length;
}

// The share of vocabulary terms in the reference that the hypothesis misses or misspells.
export function termErrorRate(ref: string, hyp: string, terms: string[]): number {
  const r = ref.toLowerCase();
  const h = hyp.toLowerCase();
  const wanted = terms.map((t) => t.toLowerCase()).filter((t) => t && r.includes(t));
  if (wanted.length === 0) return 0;
  return wanted.filter((t) => !h.includes(t)).length / wanted.length;
}

const FRAME = 10;

function frames(turns: Turn[], from: number, to: number): (string | undefined)[] {
  const out: (string | undefined)[] = new Array(Math.max(0, Math.ceil((to - from) / FRAME))).fill(undefined);
  for (const t of turns)
    for (let f = Math.max(0, Math.floor((t.start_ms - from) / FRAME)); f < Math.min(out.length, Math.ceil((t.end_ms - from) / FRAME)); f++)
      out[f] = t.speaker;
  return out;
}

// Assigns hypothesis speakers to reference speakers so that the agreeing time is largest (exhaustive for the small
// speaker counts of a meeting, greedy beyond 8).
function bestMapping(overlap: Map<string, Map<string, number>>): Map<string, string> {
  const hyps = [...overlap.keys()];
  const refs = [...new Set([...overlap.values()].flatMap((m) => [...m.keys()]))];
  const best = { score: -1, map: new Map<string, string>() };
  if (hyps.length > 8) {
    const pairs = hyps.flatMap((h) => refs.map((r) => ({ h, r, n: overlap.get(h)?.get(r) ?? 0 }))).sort((a, b) => b.n - a.n);
    const map = new Map<string, string>();
    const used = new Set<string>();
    for (const p of pairs) if (!map.has(p.h) && !used.has(p.r)) map.set(p.h, p.r), used.add(p.r);
    return map;
  }
  const walk = (i: number, used: Set<string>, map: Map<string, string>, score: number) => {
    if (i === hyps.length) {
      if (score > best.score) best.score = score, best.map = new Map(map);
      return;
    }
    const h = hyps[i]!;
    walk(i + 1, used, map, score);
    for (const r of refs) {
      if (used.has(r)) continue;
      used.add(r);
      map.set(h, r);
      walk(i + 1, used, map, score + (overlap.get(h)?.get(r) ?? 0));
      map.delete(h);
      used.delete(r);
    }
  };
  walk(0, new Set(), new Map(), 0);
  return best.map;
}

// Diarization error rate on 10 ms frames: missed, false alarm and confused speech over the reference speech, with the
// best one-to-one mapping of hypothesis to reference speakers. Frames within collarMs of a reference boundary are
// not scored.
export function der(ref: Turn[], hyp: Turn[], collarMs = 250): number {
  if (ref.length === 0) return 0;
  const from = Math.min(...ref.map((t) => t.start_ms), ...hyp.map((t) => t.start_ms));
  const to = Math.max(...ref.map((t) => t.end_ms), ...hyp.map((t) => t.end_ms));
  const r = frames(ref, from, to);
  const h = frames(hyp, from, to);
  const skip = new Array<boolean>(r.length).fill(false);
  for (const t of ref)
    for (const edge of [t.start_ms, t.end_ms])
      for (let f = Math.max(0, Math.floor((edge - collarMs - from) / FRAME)); f < Math.min(r.length, Math.ceil((edge + collarMs - from) / FRAME)); f++)
        skip[f] = true;
  const overlap = new Map<string, Map<string, number>>();
  for (let f = 0; f < r.length; f++) {
    const hs = h[f];
    const rs = r[f];
    if (skip[f] || !hs || !rs) continue;
    const m = overlap.get(hs) ?? new Map<string, number>();
    m.set(rs, (m.get(rs) ?? 0) + 1);
    overlap.set(hs, m);
  }
  const map = bestMapping(overlap);
  let speech = 0;
  let errors = 0;
  for (let f = 0; f < r.length; f++) {
    if (skip[f]) continue;
    const rs = r[f];
    const hs = h[f];
    if (rs) speech++;
    if (rs && !hs) errors++;
    else if (!rs && hs) errors++;
    else if (rs && hs && map.get(hs) !== rs) errors++;
  }
  return speech ? errors / speech : 0;
}

// The share of reference speech time whose hypothesis speaker name is the right person (no mapping: names must match).
export function namingAccuracy(ref: Turn[], hyp: Turn[]): number {
  if (ref.length === 0) return 1;
  const from = Math.min(...ref.map((t) => t.start_ms));
  const to = Math.max(...ref.map((t) => t.end_ms));
  const r = frames(ref, from, to);
  const h = frames(hyp, from, to);
  let speech = 0;
  let right = 0;
  for (let f = 0; f < r.length; f++) {
    if (!r[f]) continue;
    speech++;
    if (h[f] && h[f]!.toLowerCase() === r[f]!.toLowerCase()) right++;
  }
  return speech ? right / speech : 1;
}
