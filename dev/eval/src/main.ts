// Skowork evaluation tooling (add-evaluation-export design D5, D6). Usage:
//   node dev/eval/src/main.ts implicit <export folder>              append labels taken from the user's decisions
//   node dev/eval/src/main.ts sample <export folder> [--mail 150] [--teams 100] [--seed 1]   write sample.jsonl to label
//   node dev/eval/src/main.ts score <export folder> <run> [<run>]   write report.md (two runs: also what changed)
import { join } from 'node:path';
import { implicitLabels } from './implicit.ts';
import { appendJsonl, readJson, readJsonl, writeJsonl } from './io.ts';
import { sample } from './sample.ts';
import { report } from './score.ts';

function option(args: string[], name: string, fallback: number): number {
  const i = args.indexOf(name);
  return i >= 0 && args[i + 1] ? Number(args[i + 1]) : fallback;
}

function main(argv: string[]): number {
  const [command, folder, ...rest] = argv;
  if (!command || !folder) {
    console.error('usage: main.ts implicit|sample|score <export folder> ...');
    return 2;
  }
  const records = ['mail.jsonl', 'teams.jsonl', 'calendar.jsonl', 'meetings.jsonl'].flatMap((f) => readJsonl(join(folder, f)));
  if (command === 'implicit') {
    const existing = new Set(readJsonl(join(folder, 'labels.jsonl')).filter((l) => l.source === 'user_decision').map((l) => `${l.step}|${l.id}|${l.fact ?? ''}`));
    const fresh = implicitLabels(records).filter((l) => !existing.has(`${l.step}|${l.id}|${'fact' in l ? l.fact : ''}`));
    appendJsonl(join(folder, 'labels.jsonl'), fresh);
    console.log(`implicit labels added: ${fresh.length}`);
    return 0;
  }
  if (command === 'sample') {
    const manifest = readJson(join(folder, 'manifest.json'));
    const now = Number(manifest.window?.to ?? Date.now());
    const seed = option(rest, '--seed', 1);
    const mail = sample(readJsonl(join(folder, 'mail.jsonl')), option(rest, '--mail', 150), now, seed);
    const teams = sample(readJsonl(join(folder, 'teams.jsonl')), option(rest, '--teams', 100), now, seed);
    writeJsonl(join(folder, 'sample.jsonl'), [...mail, ...teams].map((r) => ({ id: r.id, skowork: r.skowork, user: r.user })));
    console.log(`sample: ${mail.length} mail, ${teams.length} Teams`);
    return 0;
  }
  if (command === 'score') {
    if (rest.length === 0) {
      console.error('score needs at least one run id');
      return 2;
    }
    console.log(report(folder, rest));
    return 0;
  }
  console.error(`unknown command ${command}`);
  return 2;
}

if (import.meta.main) process.exit(main(process.argv.slice(2)));

export { main };
