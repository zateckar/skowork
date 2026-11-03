// The client test fixture generated from the schema registry (design D7: one schema source for server and client).
import { KEYS, SCHEMA_VERSION } from './keys.ts';
import { Registry } from './schema.ts';

export function policyFixture(): string {
  const reg = new Registry(SCHEMA_VERSION, KEYS);
  const keys = [...reg.keys.values()]
    .sort((a, b) => a.path.localeCompare(b.path))
    .map((k) => ({ path: k.path, owner: k.owner, type: k.type, ...(k.default !== undefined && { default: k.default }) }));
  return `${JSON.stringify({ version: reg.version, keys, schema: reg.jsonSchema() }, null, 2)}\n`;
}
