// Generates the telemetry allowlists from telemetry/catalogue.yaml (add-telemetry-and-admin design D1, D2, task 2.1):
// the client's C++ header and the control plane's TypeScript module. Code names fields only through these generated
// identifiers, so a field missing from the catalogue does not compile.
// Usage: npm run telemetry:catalogue [-- --check]   (--check fails when a generated file is out of date)
import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { parse } from 'yaml';

const root = new URL('../../', import.meta.url);
const source = fileURLToPath(new URL('telemetry/catalogue.yaml', root));
const cppOut = fileURLToPath(new URL('client/src/telemetry/catalogue.gen.h', root));
const tsOut = fileURLToPath(new URL('control-plane/src/telemetry/catalogue.gen.ts', root));

const LEVELS = ['metadata', 'content'] as const;
const TYPES = ['string', 'integer', 'number', 'boolean', 'domain'] as const;
const NAME = /^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+$/;

interface Field {
  name: string;
  type: (typeof TYPES)[number];
  level: (typeof LEVELS)[number];
  purpose: string;
}
interface Event {
  name: string;
  purpose: string;
  attributes: string[];
}

function fail(msg: string): never {
  console.error(`telemetry/catalogue.yaml: ${msg}`);
  process.exit(1);
}

function fields(list: unknown, what: string): Field[] {
  if (!Array.isArray(list) || list.length === 0) fail(`${what} must be a non-empty list`);
  const seen = new Set<string>();
  return list.map((f: any, i) => {
    if (typeof f?.name !== 'string' || !NAME.test(f.name)) fail(`${what}[${i}]: name must be dotted lower case`);
    if (seen.has(f.name)) fail(`${what}: duplicate ${f.name}`);
    seen.add(f.name);
    if (!TYPES.includes(f.type)) fail(`${f.name}: type must be one of ${TYPES.join(', ')}`);
    if (!LEVELS.includes(f.level)) fail(`${f.name}: level must be one of ${LEVELS.join(', ')}`);
    if (typeof f.purpose !== 'string' || f.purpose.trim().length < 10) fail(`${f.name}: purpose is required`);
    return { name: f.name, type: f.type, level: f.level, purpose: f.purpose.trim() };
  });
}

const doc = parse(readFileSync(source, 'utf8'));
if (doc?.version !== 1) fail('version must be 1');
const resource = fields(doc.resource, 'resource');
const attributes = fields(doc.attributes, 'attributes');
for (const r of resource) {
  if (r.level !== 'metadata') fail(`${r.name}: resource fields are metadata`);
  if (attributes.some((a) => a.name === r.name)) fail(`${r.name}: both a resource field and an attribute`);
}
const known = new Set(attributes.map((a) => a.name));
const used = new Set<string>();
if (!Array.isArray(doc.events) || doc.events.length === 0) fail('events must be a non-empty list');
const eventNames = new Set<string>();
const events: Event[] = doc.events.map((e: any, i: number) => {
  if (typeof e?.name !== 'string' || !NAME.test(e.name)) fail(`events[${i}]: name must be dotted lower case`);
  if (eventNames.has(e.name)) fail(`events: duplicate ${e.name}`);
  eventNames.add(e.name);
  if (typeof e.purpose !== 'string' || !e.purpose.trim()) fail(`${e.name}: purpose is required`);
  if (!Array.isArray(e.attributes) || e.attributes.length === 0) fail(`${e.name}: attributes must be a non-empty list`);
  for (const a of e.attributes) {
    if (!known.has(a)) fail(`${e.name}: unknown attribute ${a}`);
    used.add(a);
  }
  if (new Set(e.attributes).size !== e.attributes.length) fail(`${e.name}: duplicate attribute`);
  return { name: e.name, purpose: e.purpose.trim(), attributes: e.attributes };
});
for (const a of attributes) if (!used.has(a.name)) fail(`${a.name}: not used by any event`);

const ident = (name: string) => name.replace(/\./g, '_');
const header = '// Generated from telemetry/catalogue.yaml by control-plane/scripts/telemetry-catalogue.ts. Do not edit.\n';

function cpp(): string {
  const def = (f: Field) => `    {"${f.name}", Type::${f.type}, Level::${f.level}},`;
  const lines = [
    header + '// The telemetry allowlist (add-telemetry-and-admin design D1): only these fields can be emitted.',
    '#pragma once',
    '',
    '#include <cstdint>',
    '#include <span>',
    '',
    'namespace wb::telemetry {',
    '',
    'enum class Level : std::uint8_t { off, metadata, content };',
    'enum class Type : std::uint8_t { string, integer, number, boolean, domain };',
    '',
    'struct FieldDef {',
    '    const char* name;',
    '    Type type;',
    '    Level level;',
    '};',
    '',
    `enum class Res : std::uint8_t { ${resource.map((r) => ident(r.name)).join(', ')} };`,
    'inline constexpr FieldDef kResource[] = {',
    ...resource.map(def),
    '};',
    '',
    'enum class Attr : std::uint8_t {',
    ...attributes.map((a) => `    ${ident(a.name)},`),
    '};',
    'inline constexpr FieldDef kAttrs[] = {',
    ...attributes.map(def),
    '};',
    '',
    'enum class Event : std::uint8_t {',
    ...events.map((e) => `    ${ident(e.name)},`),
    '};',
    'namespace detail {',
    ...events.map((e) => `inline constexpr Attr k_${ident(e.name)}[] = {${e.attributes.map((a) => `Attr::${ident(a)}`).join(', ')}};`),
    '}  // namespace detail',
    'struct EventDef {',
    '    const char* name;',
    '    std::span<const Attr> attrs;',
    '};',
    'inline constexpr EventDef kEvents[] = {',
    ...events.map((e) => `    {"${e.name}", detail::k_${ident(e.name)}},`),
    '};',
    '',
    'constexpr const FieldDef& def(Attr a) { return kAttrs[static_cast<int>(a)]; }',
    'constexpr const FieldDef& def(Res r) { return kResource[static_cast<int>(r)]; }',
    'constexpr const EventDef& def(Event e) { return kEvents[static_cast<int>(e)]; }',
    '',
    '}  // namespace wb::telemetry',
    '',
  ];
  return lines.join('\n');
}

function ts(): string {
  const def = (f: Field) => `  '${f.name}': { type: '${f.type}', level: '${f.level}' },`;
  return [
    header + '// The telemetry allowlist (add-telemetry-and-admin design D1), re-applied by the ingest.',
    '',
    "export type Level = 'metadata' | 'content';",
    "export type FieldType = 'string' | 'integer' | 'number' | 'boolean' | 'domain';",
    'export interface FieldDef {',
    '  readonly type: FieldType;',
    '  readonly level: Level;',
    '}',
    '',
    'export const RESOURCE = {',
    ...resource.map(def),
    '} as const satisfies Record<string, FieldDef>;',
    'export type ResourceName = keyof typeof RESOURCE;',
    '',
    'export const ATTRIBUTES = {',
    ...attributes.map(def),
    '} as const satisfies Record<string, FieldDef>;',
    'export type AttrName = keyof typeof ATTRIBUTES;',
    '',
    'export const EVENTS = {',
    ...events.map((e) => `  '${e.name}': [${e.attributes.map((a) => `'${a}'`).join(', ')}],`),
    '} as const satisfies Record<string, readonly AttrName[]>;',
    'export type EventName = keyof typeof EVENTS;',
    '',
  ].join('\n');
}

const outputs: [string, string][] = [
  [cppOut, cpp()],
  [tsOut, ts()],
];
if (process.argv.includes('--check')) {
  let stale = false;
  for (const [file, text] of outputs) {
    let current = '';
    try {
      current = readFileSync(file, 'utf8').replace(/\r\n/g, '\n');
    } catch {
      /* missing */
    }
    if (current !== text) {
      console.error(`${file} is out of date; run npm run telemetry:catalogue`);
      stale = true;
    }
  }
  if (stale) process.exit(1);
  console.log('telemetry catalogue: generated files are up to date');
} else {
  for (const [file, text] of outputs) {
    writeFileSync(file, text);
    console.log(`wrote ${file}`);
  }
}
