// The stratified sample to label (add-evaluation-export design D5, step 2): every category, old and new items, direct
// and broadcast mail, and items with and without a user decision, drawn reproducibly from a seed.
import type { Json } from './io.ts';

function rng(seed: number): () => number {
  let s = seed >>> 0 || 1;
  return () => {
    s ^= s << 13;
    s ^= s >>> 17;
    s ^= s << 5;
    return (s >>> 0) / 4294967296;
  };
}

export function stratum(rec: Json, now: number): string {
  const it = rec.item ?? {};
  const triage = rec.skowork?.triage ?? {};
  const at = Number(it.received_at ?? it.at ?? 0);
  const age = now - at > 7 * 86_400_000 ? 'old' : 'new';
  const recipients = (it.to?.length ?? 0) + (it.cc?.length ?? 0);
  const breadth = recipients > 10 ? 'broadcast' : recipients > 1 ? 'group' : 'direct';
  const decided = (rec.user?.decisions?.length ?? 0) > 0 || rec.user?.legacy ? 'decided' : 'open';
  return [triage.category || triage.outcome || 'none', age, breadth, decided].join('/');
}

// Picks up to `count` records, spread over the strata (round robin), in a fixed order for a given seed.
export function sample(records: Json[], count: number, now: number, seed = 1): Json[] {
  const next = rng(seed);
  const groups = new Map<string, Json[]>();
  for (const r of records) {
    const k = stratum(r, now);
    groups.set(k, [...(groups.get(k) ?? []), r]);
  }
  const keys = [...groups.keys()].sort();
  for (const k of keys) {
    const g = groups.get(k)!;
    for (let i = g.length - 1; i > 0; i--) {
      const j = Math.floor(next() * (i + 1));
      [g[i], g[j]] = [g[j]!, g[i]!];
    }
  }
  const out: Json[] = [];
  for (let round = 0; out.length < count; round++) {
    let added = false;
    for (const k of keys) {
      const g = groups.get(k)!;
      if (round < g.length && out.length < count) out.push(g[round]!), (added = true);
    }
    if (!added) break;
  }
  return out;
}
