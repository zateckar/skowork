// The public, version-one publication envelope. This parser is deliberately not a route:
// authorization, revision barriers and capability negotiation must precede exposure.
import { ApiError } from '../http.ts';

export const PUBLICATION_SOURCE_KINDS = ['threads', 'files', 'mail', 'calendar', 'meetings', 'notes'] as const;
export type PublicationSourceKind = typeof PUBLICATION_SOURCE_KINDS[number];
export interface PublicationEvidence { id: string; title: string; text: string }
export interface PublicationEntry { id: string; text: string; source_kinds: PublicationSourceKind[]; evidence: PublicationEvidence[] }
export interface PublicationInput {
  expected_revision: number;
  operation_id: string;
  title: string;
  groups: string[];
  entries: PublicationEntry[];
}

const INVALID_ID_CHAR = /[^A-Za-z0-9._:-]/;
const KINDS = new Set<string>(PUBLICATION_SOURCE_KINDS);

function invalid(): never {
  // Never include submitted content in errors or logs.
  throw new ApiError(400, 'INVALID_REQUEST', 'Invalid publication payload.');
}

function record(value: unknown, keys: readonly string[]): Record<string, unknown> {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return invalid();
  const obj = value as Record<string, unknown>;
  const actual = Object.keys(obj);
  if (actual.length !== keys.length || actual.some((key) => !keys.includes(key))) return invalid();
  return obj;
}

function wellFormed(value: string): boolean {
  for (let i = 0; i < value.length; ++i) {
    const c = value.charCodeAt(i);
    if (c >= 0xd800 && c <= 0xdbff) {
      const next = value.charCodeAt(++i);
      if (!(next >= 0xdc00 && next <= 0xdfff)) return false;
    } else if (c >= 0xdc00 && c <= 0xdfff) return false;
  }
  return true;
}

function label(value: unknown): string {
  if (typeof value !== 'string' || value.length > 300 || !value.trim() || !wellFormed(value)) return invalid();
  return value;
}

function id(value: unknown): string {
  if (typeof value !== 'string' || value.length < 1 || value.length > 200 || INVALID_ID_CHAR.test(value)) return invalid();
  return value;
}

function content(value: unknown, maxBytes: number): string {
  if (typeof value !== 'string' || !value.trim() || !wellFormed(value)) return invalid();
  if (Buffer.byteLength(value, 'utf8') > maxBytes) throw new ApiError(413, 'TOO_LARGE', 'The publication is too large.');
  return value;
}

/** Parse an already JSON-decoded body without retaining any unrecognized/private fields. */
export function parsePublicationInput(value: unknown, maxBytes: number): PublicationInput {
  if (!Number.isSafeInteger(maxBytes) || maxBytes < 1) throw new RangeError('Invalid publication size limit.');
  const obj = record(value, ['expected_revision', 'operation_id', 'title', 'groups', 'entries']);
  if (!Number.isSafeInteger(obj.expected_revision) || (obj.expected_revision as number) < 0) return invalid();
  const groups = obj.groups;
  const entries = obj.entries;
  if (!Array.isArray(groups) || groups.length < 1 || groups.length > 100 ||
      !Array.isArray(entries) || entries.length < 1 || entries.length > 500) return invalid();
  const parsedGroups = groups.map(label);
  if (new Set(parsedGroups).size !== parsedGroups.length) return invalid();
  const usedIds = new Set<string>();
  let evidenceCount = 0;
  let contentBytes = 0;
  const boundedContent = (value: unknown): string => {
    const text = content(value, maxBytes);
    contentBytes += Buffer.byteLength(text, 'utf8');
    if (contentBytes > maxBytes) throw new ApiError(413, 'TOO_LARGE', 'The publication is too large.');
    return text;
  };
  const parsedEntries: PublicationEntry[] = entries.map((value: unknown) => {
    const entry = record(value, ['id', 'text', 'source_kinds', 'evidence']);
    const entryId = id(entry.id);
    if (usedIds.has(entryId)) return invalid();
    usedIds.add(entryId);
    if (!Array.isArray(entry.source_kinds) || entry.source_kinds.length < 1 || entry.source_kinds.length > KINDS.size ||
        !entry.source_kinds.every((kind: unknown) => typeof kind === 'string' && KINDS.has(kind)) ||
        new Set(entry.source_kinds).size !== entry.source_kinds.length || !Array.isArray(entry.evidence)) return invalid();
    evidenceCount += entry.evidence.length;
    if (evidenceCount > 2000) return invalid();
    const evidence = entry.evidence.map((value: unknown) => {
      const item = record(value, ['id', 'title', 'text']);
      const evidenceId = id(item.id);
      if (usedIds.has(evidenceId)) return invalid();
      usedIds.add(evidenceId);
      return { id: evidenceId, title: label(item.title), text: boundedContent(item.text) };
    });
    return { id: entryId, text: boundedContent(entry.text), source_kinds: [...entry.source_kinds] as PublicationSourceKind[], evidence };
  });
  const parsed: PublicationInput = {
    expected_revision: obj.expected_revision as number, operation_id: id(obj.operation_id), title: label(obj.title),
    groups: parsedGroups, entries: parsedEntries,
  };
  if (Buffer.byteLength(JSON.stringify(parsed), 'utf8') > maxBytes)
    throw new ApiError(413, 'TOO_LARGE', 'The publication is too large.');
  return parsed;
}

/** Re-run with freshly compiled owner policy at final commit, not just before embedding. */
export function requirePermittedPublicationSources(input: PublicationInput, permitted: ReadonlySet<string>): void {
  for (const entry of input.entries)
    if (entry.source_kinds.some((kind) => !permitted.has(kind)))
      throw new ApiError(403, 'SOURCE_NOT_PERMITTED', 'The publication includes a Source kind that policy does not permit.');
}
