// Writes the generated policy JSON Schema and key list into the client's test fixtures, so the client policy parser is
// tested against the same single schema source as the server validator (add-policy-and-sandbox design D7, task 2.1).
// Usage: npm run policy:schema [-- --check]
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname } from 'node:path';
import { policyFixture } from '../src/policy/fixture.ts';

const out = fileURLToPath(new URL('../../client/tests/fixtures/policy/schema.json', import.meta.url));
const text = policyFixture();
if (process.argv.includes('--check')) {
  let current = '';
  try {
    current = readFileSync(out, 'utf8');
  } catch {
    /* missing */
  }
  if (current !== text) {
    console.error(`${out} is out of date; run npm run policy:schema`);
    process.exit(1);
  }
} else {
  mkdirSync(dirname(out), { recursive: true });
  writeFileSync(out, text);
  console.log(`wrote ${out}`);
}
