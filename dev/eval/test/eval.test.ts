// dev/eval on the synthetic export (fixtures/synthetic-export holds no real data).
import assert from 'node:assert/strict';
import { cpSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { implicitLabels } from '../src/implicit.ts';
import { readJsonl } from '../src/io.ts';
import { der, namingAccuracy, precisionRecall, termErrorRate, wer } from '../src/metrics.ts';
import { sample, stratum } from '../src/sample.ts';
import { changedItems, loadLabels, report, scoreRun } from '../src/score.ts';

const fixture = join(import.meta.dirname, '..', 'fixtures', 'synthetic-export');

function copy(): string {
  const dir = mkdtempSync(join(tmpdir(), 'skowork-eval-'));
  cpSync(fixture, dir, { recursive: true });
  return dir;
}

test('word error rate counts substitutions over the reference', () => {
  assert.equal(wer('a b c', 'a x c'), 1 / 3);
  assert.equal(wer('Hello, World!', 'hello world'), 0);
  assert.equal(wer('', ''), 0);
});

test('term error rate looks only at terms in the reference', () => {
  assert.equal(termErrorRate('Petra Malá owns Phoenix', 'petra mala owns phoenix', ['Petra Malá', 'Phoenix', 'Falcon']), 0.5);
});

test('precision and recall', () => {
  assert.deepEqual(precisionRecall([true, true, false, false], [true, false, true, false]), { precision: 0.5, recall: 0.5 });
});

test('diarization error rate is zero for identical and swapped labels', () => {
  const ref = [
    { start_ms: 0, end_ms: 5000, speaker: 'jan' },
    { start_ms: 5000, end_ms: 10000, speaker: 'petra' },
  ];
  assert.equal(der(ref, ref, 0), 0);
  const swapped = [
    { start_ms: 0, end_ms: 5000, speaker: 's2' },
    { start_ms: 5000, end_ms: 10000, speaker: 's1' },
  ];
  assert.equal(der(ref, swapped, 0), 0);
  const merged = [{ start_ms: 0, end_ms: 10000, speaker: 's1' }];
  assert.equal(der(ref, merged, 0), 0.5);
  assert.equal(namingAccuracy(ref, [{ start_ms: 0, end_ms: 10000, speaker: 'Jan' }]), 0.5);
});

test('implicit labels follow the user decisions', () => {
  const labels = implicitLabels(readJsonl(join(fixture, 'mail.jsonl')));
  const byId = new Map(labels.filter((l) => l.step === 'triage').map((l) => [l.id, l]));
  assert.equal((byId.get('mail:a') as any).action, 'required');
  assert.equal((byId.get('mail:b') as any).action, 'info', 'a legacy dismissal means no action was needed');
  const fact = labels.find((l) => l.step === 'knowledge') as any;
  assert.equal(fact.keep, 'wrong');
  assert.equal(fact.fact, 'Petr likes coffee');
});

test('the sample is reproducible and spreads over strata', () => {
  const recs = readJsonl(join(fixture, 'mail.jsonl'));
  const now = 1790812800000;
  const a = sample(recs, 3, now, 7).map((r) => r.id);
  const b = sample(recs, 3, now, 7).map((r) => r.id);
  assert.deepEqual(a, b);
  const strata = new Set(sample(recs, 3, now, 7).map((r) => stratum(r, now)));
  assert.equal(strata.size, 3);
});

test('scoring gives the design D6 triage metrics and the failure rate from run.json', () => {
  const labels = loadLabels(fixture);
  const a = scoreRun(fixture, 'run_a', labels);
  assert.equal(a.metrics.action_required_recall, 1);
  assert.equal(a.metrics.action_required_precision, 0.5);
  assert.equal(a.metrics.stale_marked_required, 1);
  assert.equal(a.failure_rate, 0);
  assert.equal(a.tokens_per_item, 250);
  const b = scoreRun(fixture, 'run_b', labels);
  assert.equal(b.failure_rate, 0.25);
  assert.equal(b.metrics.stale_marked_required, 0);
  assert.deepEqual(changedItems(fixture, 'run_a', 'run_b').sort(), ['mail:b', 'mail:d']);
});

test('report.md compares two runs', () => {
  const dir = copy();
  try {
    const text = report(dir, ['run_a', 'run_b']);
    assert.match(text, /## triage — run run_b/);
    assert.match(text, /Items whose result changed: 2/);
    assert.match(readFileSync(join(dir, 'report.md'), 'utf8'), /action_required_precision/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
