// Reading and writing the JSON-lines files of an evaluation export.
import { appendFileSync, existsSync, readFileSync, writeFileSync } from 'node:fs';

export type Json = Record<string, any>;

export function readJsonl(file: string): Json[] {
  if (!existsSync(file)) return [];
  const out: Json[] = [];
  for (const line of readFileSync(file, 'utf8').split(/\r?\n/)) {
    if (!line.trim()) continue;
    out.push(JSON.parse(line) as Json);
  }
  return out;
}

export function writeJsonl(file: string, records: unknown[]): void {
  writeFileSync(file, records.map((r) => JSON.stringify(r)).join('\n') + (records.length ? '\n' : ''));
}

export function appendJsonl(file: string, records: unknown[]): void {
  if (records.length) appendFileSync(file, records.map((r) => JSON.stringify(r)).join('\n') + '\n');
}

export function readJson(file: string): Json {
  return JSON.parse(readFileSync(file, 'utf8').split(/\r?\n/)[0] ?? '{}') as Json;
}
