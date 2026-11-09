import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { matchesToolPattern, simulateToolDecision, type SimulatedCall } from '../src/policy/tool-decision.ts';
import { startHarness } from './helpers.ts';

test('tool simulation agrees with the shared desktop decision fixtures', async () => {
  const cases = JSON.parse(await readFile(new URL('../../client/tests/fixtures/policy/tool-decisions.json', import.meta.url), 'utf8')) as
    (SimulatedCall & { name: string; policy: Record<string, unknown>; src?: Record<string, string>; decision: string; rule: string; source: string })[];
  for (const c of cases) {
    const result = simulateToolDecision(c.policy, c.src ?? {}, c);
    assert.equal(result.decision, c.decision, c.name);
    assert.equal(result.rule, c.rule, c.name);
    assert.equal(result.source, c.source, c.name);
    assert.ok(result.conditions.length > 0, c.name);
  }
});

test('tool pattern matching respects separators, ASCII case and wildcard bounds', () => {
  assert.equal(matchesToolPattern('mcp:corp/*', 'mcp:corp/a/b'), false);
  assert.equal(matchesToolPattern('MCP:corp/**', 'mcp:corp/a/b'), true);
  assert.equal(matchesToolPattern('a.?', 'a.B'), true);
  assert.equal(matchesToolPattern('*'.repeat(4097), 'a'), false);
});

test('disabled computer mode overrides an allow rule', () => {
  const result = simulateToolDecision({ tools: { rules: { 'computer.click': 'allow' } } }, {},
    { tool: 'computer.click', risk: 'external', unattended: false });
  assert.equal(result.decision, 'deny');
  assert.equal(result.rule, 'tools.computer.mode');
  assert.equal(result.source, 'default');
});

test('trace explains an explicit unattended rule lifting a tool floor', () => {
  const result = simulateToolDecision({ tools: { computer: { mode: 'allow' }, risk: { external: 'allow' } },
    unattended: { rules: { 'computer.click': 'allow' } } }, {},
  { tool: 'computer.click', risk: 'external', unattended: true });
  assert.equal(result.decision, 'allow');
  const floor = result.steps.find((step) => step.stage === 'tool floor');
  assert.equal(floor?.considered?.rule, 'tool:computer.click');
  assert.match(floor?.considered?.reason ?? '', /lifted by the explicit unattended allow/);
  assert.equal(result.steps.find((step) => step.stage === 'unattended rule')?.considered?.rule, 'unattended.rules["computer.click"]');
});

test('simulation API composes groups and guardrails before explaining a tool', async () => {
  const h = await startHarness();
  try {
    const admin = await h.token('admin1');
    const root = '/api/v1/admin/policy';
    const draft = await h.call('PUT', `${root}/groups/engineering/draft`, admin, {
      document: { tools: { computer: { mode: 'allow' }, rules: { 'computer.click': 'allow' } } }, reason: 'test', priority: 1,
    });
    assert.equal(draft.status, 200);
    const guard = await h.call('PUT', `${root}/guardrail/draft`, admin, {
      document: { tools: { rules: { 'computer.click': 'deny' } } }, reason: 'test',
    });
    assert.equal(guard.status, 200);
    const request = { groups: ['engineering'], drafts: true, tool_call: { tool: 'computer.click', risk: 'external', unattended: false } };
    const response = await h.call('POST', `${root}/simulate`, admin, request);
    assert.equal(response.status, 200);
    assert.equal(response.body.tool_decision.decision, 'deny');
    assert.equal(response.body.tool_decision.rule, 'tools.rules["computer.click"]');
    assert.equal(response.body.tool_decision.source, 'guardrail');
    assert.ok(response.body.tool_decision.conditions.length);
    const invalid = await h.call('POST', `${root}/simulate`, admin, { ...request, tool_call: { ...request.tool_call, tool: 'computer.*' } });
    assert.equal(invalid.status, 400);
  } finally {
    await h.close();
  }
});
