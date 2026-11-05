// Skill package validation (control-plane/skill-registry Skill Package Validation; add-skills design D6): the zip
// layout, the SKILL.md frontmatter (a small YAML subset) and the required fields.
import { ApiError } from '../http.ts';
import { parseVersion } from '../fleet/fleet.ts';
import { readZip, ZipError, type ZipEntry } from './zip.ts';

export const RISKS = ['read', 'write-local', 'external', 'destructive'] as const;
export const NAME = /^[a-z0-9]+(?:-[a-z0-9]+)*$/;
const SEMVER = /^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/;
const FOLDERS = ['scripts', 'references', 'assets'];
const ROOT_FILES = /^(skill\.md|readme(\.md|\.txt)?|licen[cs]e(\.md|\.txt)?)$/i;

export interface PackageLimits {
  maxPackageBytes: number;
  maxEntries: number;
  maxUnpackedBytes: number;
}

/** A frontmatter value: a scalar, a list, or a nested block kept as its source text. */
export type FrontValue = string | string[] | { block: string };

export interface Manifest {
  name: string;
  description: string;
  version: string;
  tools: string[];
  risk: (typeof RISKS)[number];
  network: 'none' | 'proxy';
  requires_mcp: string[];
  /** Every frontmatter field, unknown ones included, as parsed. */
  frontmatter: Record<string, FrontValue>;
}

export interface SkillPackage {
  manifest: Manifest;
  body: string;
  /** Entries relative to the Skill folder (the top-level folder, if any, removed). */
  files: ZipEntry[];
}

const invalid = (field: string, message: string) => new ApiError(400, 'VALIDATION_FAILED', message, { field });

function unquote(v: string): string {
  const t = v.trim();
  if (t.length >= 2 && t.startsWith('"') && t.endsWith('"')) {
    try {
      return JSON.parse(t) as string;
    } catch {
      return t.slice(1, -1);
    }
  }
  if (t.length >= 2 && t.startsWith("'") && t.endsWith("'")) return t.slice(1, -1).replace(/''/g, "'");
  return t.replace(/\s+#.*$/, '');
}

function inlineList(v: string): string[] {
  const inner = v.trim().slice(1, -1).trim();
  if (!inner) return [];
  return inner.split(',').map(unquote).filter((s) => s !== '');
}

/**
 * Splits SKILL.md into frontmatter and body. The frontmatter subset: `key: scalar`, `key: [a, b]`, a block list of
 * `- item` lines, folded or literal scalars (`>` / `|`), and any other indented block kept as text.
 */
export function parseSkillMd(text: string): { frontmatter: Record<string, FrontValue>; body: string } {
  const src = text.replace(/^\uFEFF/, '').replace(/\r\n?/g, '\n');
  if (!src.startsWith('---\n')) throw invalid('frontmatter', 'SKILL.md must start with a YAML frontmatter block (---).');
  const end = src.indexOf('\n---', 3);
  if (end < 0 || (src[end + 4] !== undefined && src[end + 4] !== '\n')) throw invalid('frontmatter', 'The frontmatter block of SKILL.md is not closed with ---.');
  const body = src.slice(end + 4).replace(/^\n/, '');
  return { frontmatter: parseMapping(src.slice(4, end + 1).split('\n')), body };
}

function parseMapping(lines: string[]): Record<string, FrontValue> {
  const out: Record<string, FrontValue> = {};
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i]!;
    if (!line.trim() || line.trimStart().startsWith('#')) continue;
    if (/^\s/.test(line)) throw invalid('frontmatter', `Line ${i + 2} of the frontmatter is indented without a key.`);
    const m = /^([A-Za-z_][A-Za-z0-9_-]*)\s*:(.*)$/.exec(line);
    if (!m) throw invalid('frontmatter', `Line ${i + 2} of the frontmatter is not "key: value".`);
    const key = m[1]!;
    if (key in out) throw invalid(key, `The frontmatter field ${key} appears twice.`);
    const rest = m[2]!.trim();
    const block: string[] = [];
    while (i + 1 < lines.length && (lines[i + 1] === '' || /^\s/.test(lines[i + 1]!) || (rest === '' && lines[i + 1]!.startsWith('- ')))) block.push(lines[++i]!);
    while (block.length && block[block.length - 1]!.trim() === '') block.pop();
    if (rest === '' || rest === '|' || rest === '>' || /^[|>][+-]?$/.test(rest)) {
      if (rest !== '' && block.length) {
        const indent = Math.min(...block.filter((l) => l.trim()).map((l) => /^\s*/.exec(l)![0].length));
        const parts = block.map((l) => l.slice(indent));
        out[key] = rest.startsWith('|') ? parts.join('\n') : parts.join(' ').replace(/\s+/g, ' ').trim();
      } else if (block.length && block.every((l) => !l.trim() || /^\s*- /.test(l))) {
        out[key] = block.filter((l) => l.trim()).map((l) => unquote(l.replace(/^\s*- /, '')));
      } else {
        out[key] = block.length ? { block: block.join('\n') } : '';
      }
    } else if (rest.startsWith('[') && rest.endsWith(']')) {
      out[key] = inlineList(rest);
    } else {
      if (block.length) throw invalid(key, `The value of ${key} continues on indented lines; use a block scalar (|).`);
      out[key] = unquote(rest);
    }
  }
  return out;
}

const scalar = (fm: Record<string, FrontValue>, key: string): string | undefined => {
  const v = fm[key];
  if (v === undefined) return undefined;
  if (typeof v !== 'string') throw invalid(key, `${key} must be a single value.`);
  return v.trim();
};

const listOf = (fm: Record<string, FrontValue>, key: string): string[] => {
  const v = fm[key];
  if (v === undefined || v === '') return [];
  if (typeof v === 'string') return v.split(',').map((s) => s.trim()).filter(Boolean);
  if (Array.isArray(v)) return v;
  throw invalid(key, `${key} must be a list.`);
};

/** `metadata.version`, where agentskills.io packages declare it; '' when absent. */
function metadataVersion(fm: Record<string, FrontValue>): string {
  const meta = fm.metadata;
  if (!meta || typeof meta !== 'object' || Array.isArray(meta)) return '';
  const lines = meta.block.split('\n');
  const indent = Math.min(...lines.filter((l) => l.trim()).map((l) => /^\s*/.exec(l)![0].length));
  let inner: Record<string, FrontValue>;
  try {
    inner = parseMapping(lines.map((l) => l.slice(indent)));
  } catch {
    return '';
  }
  return typeof inner.version === 'string' ? inner.version.trim() : '';
}

/** Validates the frontmatter: name, description and a semantic version are required in the registry. */
export function manifestOf(frontmatter: Record<string, FrontValue>): Manifest {
  const name = scalar(frontmatter, 'name') ?? '';
  if (!name) throw invalid('name', 'SKILL.md needs a name.');
  if (!NAME.test(name) || name.length > 64) throw invalid('name', `The name "${name}" must be lower-case kebab-case, at most 64 characters.`);
  const description = scalar(frontmatter, 'description') ?? '';
  if (!description) throw invalid('description', 'SKILL.md needs a description.');
  if (description.length > 1024) throw invalid('description', 'The description is longer than 1024 characters.');
  const version = scalar(frontmatter, 'version') || metadataVersion(frontmatter);
  if (!version) throw invalid('version', 'SKILL.md needs a version (semantic, for example 1.0.0).');
  if (!SEMVER.test(version) || !parseVersion(version)) throw invalid('version', `The version "${version}" is not a semantic version.`);
  const risk = (scalar(frontmatter, 'risk') ?? 'read') as Manifest['risk'];
  if (!RISKS.includes(risk)) throw invalid('risk', `risk must be one of ${RISKS.join(', ')}.`);
  const network = (scalar(frontmatter, 'network') ?? 'proxy') as Manifest['network'];
  if (network !== 'none' && network !== 'proxy') throw invalid('network', 'network must be none or proxy.');
  return { name, description, version, risk, network, tools: listOf(frontmatter, 'tools'), requires_mcp: listOf(frontmatter, 'requires_mcp'), frontmatter };
}

/** Reads and validates a Skill package (zip bytes). */
export function readPackage(bytes: Buffer, limits: PackageLimits): SkillPackage {
  if (bytes.length === 0) throw new ApiError(400, 'INVALID_REQUEST', 'The package is empty.');
  if (bytes.length > limits.maxPackageBytes) throw new ApiError(413, 'TOO_LARGE', `The package is larger than the limit of ${limits.maxPackageBytes} bytes.`);
  let entries: ZipEntry[];
  try {
    entries = readZip(bytes, { maxEntries: limits.maxEntries, maxUnpackedBytes: limits.maxUnpackedBytes });
  } catch (err) {
    if (err instanceof ZipError) throw invalid('package', err.message);
    throw err;
  }
  // SKILL.md at the root, or inside one top-level folder.
  let prefix = '';
  if (!entries.some((e) => e.path === 'SKILL.md')) {
    const tops = new Set(entries.map((e) => e.path.split('/')[0]!));
    const only = tops.size === 1 ? [...tops][0]! : '';
    if (!only || !entries.some((e) => e.path === `${only}/SKILL.md`)) throw invalid('package', 'SKILL.md must be at the root of the archive or inside one top-level folder.');
    prefix = `${only}/`;
  }
  const files = entries.map((e) => ({ path: e.path.slice(prefix.length), data: e.data }));
  for (const f of files) {
    const top = f.path.split('/')[0]!;
    const nested = f.path.includes('/');
    if (nested ? !FOLDERS.includes(top) : !ROOT_FILES.test(top)) throw invalid('package', `The package has an unknown entry "${f.path}". Allowed: SKILL.md, README, LICENSE, scripts/, references/, assets/.`);
  }
  const md = files.find((f) => f.path === 'SKILL.md')!;
  const { frontmatter, body } = parseSkillMd(md.data.toString('utf8'));
  return { manifest: manifestOf(frontmatter), body, files: files.sort((a, b) => a.path.localeCompare(b.path)) };
}
