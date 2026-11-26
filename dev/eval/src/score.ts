// Scores replays of an export against its labels and writes report.md (add-evaluation-export design D6).
import { existsSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { readJson, readJsonl, type Json } from './io.ts';
import { isLabel, type FactLabel, type Label, type MeetingLabel, type TriageLabel } from './labels.ts';
import { der, namingAccuracy, precisionRecall, termErrorRate, wer } from './metrics.ts';

export interface RunScore {
  run: string;
  step: string;
  instruction_id: string;
  items: number;
  failure_rate: number;
  failures_by_kind: Record<string, number>;
  tokens_per_item: number;
  metrics: Record<string, number>;
  by_source: Record<string, Record<string, number>>;
  changed?: string[];
}

const pct = (x: number) => `${(x * 100).toFixed(1)}%`;

function triageMetrics(labels: TriageLabel[], results: Map<string, Json>): Record<string, number> {
  const scored = labels.filter((l) => results.has(l.id));
  const out: Record<string, number> = { labelled: scored.length };
  const withAction = scored.filter((l) => l.action);
  if (withAction.length) {
    const pr = precisionRecall(
      withAction.map((l) => results.get(l.id)!.action === 'required'),
      withAction.map((l) => l.action === 'required'),
    );
    out.action_required_precision = pr.precision;
    out.action_required_recall = pr.recall;
  }
  const withRelevant = scored.filter((l) => typeof l.relevant === 'boolean');
  if (withRelevant.length)
    out.relevant_accuracy = withRelevant.filter((l) => (results.get(l.id)!.outcome === 'relevant') === l.relevant).length / withRelevant.length;
  const withCategory = scored.filter((l) => l.category);
  if (withCategory.length) out.category_accuracy = withCategory.filter((l) => results.get(l.id)!.category === l.category).length / withCategory.length;
  const stale = scored.filter((l) => l.urgency === 'stale');
  if (stale.length) out.stale_marked_required = stale.filter((l) => results.get(l.id)!.action === 'required').length / stale.length;
  return out;
}

function agendaMetrics(labels: Label[], results: Map<string, Json>): Record<string, number> {
  let tp = 0;
  let fp = 0;
  let fn = 0;
  for (const l of labels) {
    if (l.step !== 'agenda' || !results.has(l.id)) continue;
    const got: Json[] = results.get(l.id)!.candidates ?? [];
    const want = l.commitments;
    // A candidate matches a commitment when one quote contains the other (the agenda corpus rule).
    const matched = new Set<number>();
    for (const c of got) {
      const i = want.findIndex((w, k) => !matched.has(k) && (String(c.quote).includes(w.quote) || w.quote.includes(String(c.quote))));
      if (i >= 0) matched.add(i), tp++;
      else fp++;
    }
    fn += want.length - matched.size;
  }
  return { precision: tp + fp ? tp / (tp + fp) : 1, recall: tp + fn ? tp / (tp + fn) : 1, labelled: tp + fn };
}

function knowledgeMetrics(labels: FactLabel[], results: Map<string, Json>): Record<string, number> {
  const norm = (s: string) => s.toLowerCase().replace(/\s+/g, ' ').trim();
  let stored = 0;
  let trivialOrWrong = 0;
  let important = 0;
  let importantKept = 0;
  for (const l of labels) {
    const rels: Json[] = results.get(l.id)?.relations ?? [];
    const kept = rels.some((r) => [r.statement, `${r.subject} ${r.predicate} ${r.object}`].some((s) => s && norm(String(s)).includes(norm(l.fact))));
    if (kept) stored++;
    if (kept && (l.keep === 'trivial' || l.keep === 'wrong')) trivialOrWrong++;
    if (l.keep === 'important') important++, kept && importantKept++;
  }
  return { labelled: labels.length, trivial_or_wrong_share: stored ? trivialOrWrong / stored : 0, important_kept: important ? importantKept / important : 1 };
}

function meetingMetrics(labels: MeetingLabel[], results: Map<string, Json>): Record<string, number> {
  const ders: number[] = [];
  const wers: number[] = [];
  const terms: number[] = [];
  const naming: number[] = [];
  let unnamed = 0;
  let speech = 0;
  for (const l of labels) {
    const r = results.get(l.id);
    if (!r) continue;
    const segs: Json[] = r.segments ?? [];
    const names: Record<string, string> = r.speakers ?? {};
    for (const s of segs) {
      const d = Number(s.end_ms) - Number(s.start_ms);
      speech += d;
      if (/^Speaker \d+$/.test(names[s.speaker] ?? '')) unnamed += d;
    }
    for (const ref of l.reference ?? []) {
      const inside = segs.filter((s) => s.end_ms > ref.start_ms && s.start_ms < ref.end_ms);
      wers.push(wer(ref.text, inside.map((s) => s.text).join(' ')));
      if (l.terms?.length) terms.push(termErrorRate(ref.text, inside.map((s) => s.text).join(' '), l.terms));
      const hyp = inside.map((s) => ({ start_ms: Number(s.start_ms), end_ms: Number(s.end_ms), speaker: String(s.speaker) }));
      ders.push(der(ref.turns, hyp));
      naming.push(namingAccuracy(ref.turns, hyp.map((t) => ({ ...t, speaker: names[t.speaker] ?? t.speaker }))));
    }
  }
  const mean = (xs: number[]) => (xs.length ? xs.reduce((a, b) => a + b, 0) / xs.length : NaN);
  return { der: mean(ders), naming_accuracy: mean(naming), wer: mean(wers), term_error_rate: mean(terms), unnamed_share: speech ? unnamed / speech : NaN };
}

export function scoreRun(folder: string, run: string, labels: Label[]): RunScore {
  const meta = readJson(join(folder, 'runs', run, 'run.json'));
  const results = new Map<string, Json>();
  for (const r of readJsonl(join(folder, 'runs', run, 'results.jsonl'))) if (r.result) results.set(String(r.id), r.result);
  const failures: Record<string, number> = meta.failures_by_kind ?? {};
  const failed = Object.values(failures).reduce((a, b) => a + Number(b), 0);
  const items = Number(meta.items ?? 0);
  const step = String(meta.step);
  const pick = <T extends Label>(s: string, src?: string) => labels.filter((l) => l.step === s && (!src || l.source === src)) as T[];
  const score = (src?: string): Record<string, number> =>
    step === 'triage'
      ? triageMetrics(pick<TriageLabel>('triage', src), results)
      : step === 'agenda'
        ? agendaMetrics(pick('agenda', src), results)
        : step === 'knowledge'
          ? knowledgeMetrics(pick<FactLabel>('knowledge', src), results)
          : meetingMetrics(pick<MeetingLabel>('meeting', src), results);
  return {
    run,
    step,
    instruction_id: String(meta.instruction_id ?? ''),
    items,
    failure_rate: items ? failed / items : 0,
    failures_by_kind: failures,
    tokens_per_item: items ? (Number(meta.tokens_in ?? 0) + Number(meta.tokens_out ?? 0)) / items : 0,
    metrics: score(),
    by_source: { user_decision: score('user_decision'), drafted: score('drafted'), user: score('user') },
  };
}

// Items whose result changed between two runs of the same step.
export function changedItems(folder: string, a: string, b: string): string[] {
  const read = (run: string) => new Map(readJsonl(join(folder, 'runs', run, 'results.jsonl')).map((r) => [String(r.id), JSON.stringify(r.result ?? r.error)]));
  const ra = read(a);
  const rb = read(b);
  return [...ra.keys()].filter((k) => rb.has(k) && rb.get(k) !== ra.get(k));
}

export function loadLabels(folder: string): Label[] {
  return readJsonl(join(folder, 'labels.jsonl')).filter(isLabel);
}

export function report(folder: string, runs: string[]): string {
  const labels = loadLabels(folder);
  const scores = runs.map((r) => scoreRun(folder, r, labels));
  if (scores.length === 2 && scores[0]!.step === scores[1]!.step) scores[1]!.changed = changedItems(folder, runs[0]!, runs[1]!);
  const lines = ['# Evaluation report', '', `Export: ${folder}`, `Labels: ${labels.length}`, ''];
  for (const s of scores) {
    lines.push(`## ${s.step} — run ${s.run}`, '', `Instructions: ${s.instruction_id} · items ${s.items} · failures ${pct(s.failure_rate)} · tokens per item ${s.tokens_per_item.toFixed(0)}`, '');
    lines.push('| Metric | All labels | User decisions | Drafted | User |', '|---|---|---|---|---|');
    for (const [k, v] of Object.entries(s.metrics)) {
      const fmt = (x: number | undefined) => (x === undefined || Number.isNaN(x) ? '—' : k === 'labelled' ? String(x) : pct(x));
      lines.push(`| ${k} | ${fmt(v)} | ${fmt(s.by_source.user_decision?.[k])} | ${fmt(s.by_source.drafted?.[k])} | ${fmt(s.by_source.user?.[k])} |`);
    }
    if (Object.keys(s.failures_by_kind).length) lines.push('', `Failures by kind: ${JSON.stringify(s.failures_by_kind)}`);
    if (s.changed) lines.push('', `Items whose result changed: ${s.changed.length}`, ...s.changed.slice(0, 50).map((id) => `- ${id}`));
    lines.push('');
  }
  const text = lines.join('\n');
  if (existsSync(folder)) writeFileSync(join(folder, 'report.md'), text);
  return text;
}
