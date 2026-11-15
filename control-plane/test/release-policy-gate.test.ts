import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { parse } from 'yaml';

test('release artifacts wait for the policy catalog checks', async () => {
  const workflow = parse(await readFile(new URL('../../.github/workflows/release.yml', import.meta.url), 'utf8'));
  const jobs = workflow.jobs;
  for (const name of ['control-plane-image', 'desktop-zip'])
    assert.ok(jobs[name].needs.includes('policy-catalog'), `${name} can publish without policy checks`);
  const commands = jobs['policy-catalog'].steps.map((step: { run?: string }) => step.run ?? '').join('\n');
  assert.match(commands, /npm run policy:schema -- --check/);
  assert.match(commands, /node --test .*policy-tool-catalog\.test\.ts/);
  assert.match(commands, /npm run check/);
  assert.match(commands, /npm run admin:build/);
});
