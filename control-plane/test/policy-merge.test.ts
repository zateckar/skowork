// Effective-policy merge, table-driven (add-policy-and-sandbox task 2.3; control-plane/policy-service merge and
// guardrail requirements, design D2, D9, D10, D11).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { KEYS, SCHEMA_VERSION } from '../src/policy/keys.ts';
import { Registry } from '../src/policy/schema.ts';
import { compile, globMatch, type GroupDoc } from '../src/policy/merge.ts';
import { policyFixture } from '../src/policy/fixture.ts';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const reg = new Registry(SCHEMA_VERSION, KEYS);

function doc(group: string, priority: number, content: unknown): GroupDoc {
  const { flat, errors } = reg.validate(content);
  assert.deepEqual(errors, [], `fixture ${group} is valid`);
  return { group, priority, version: 1, flat };
}
function guard(content: unknown) {
  const { flat, errors } = reg.validate(content, { guardrail: true });
  assert.deepEqual(errors, [], 'guardrail fixture is valid');
  return { version: 1, flat };
}

interface Case {
  name: string;
  groups: GroupDoc[];
  guardrail?: unknown;
  mcp?: unknown[];
  expect: (c: ReturnType<typeof compile>) => void;
}

const cases: Case[] = [
  {
    name: 'scalar: highest-priority Group decides (Scalar Precedence: Two groups)',
    groups: [doc('power-users', 10, { tools: { shell: { mode: 'off' } } }), doc('developers', 20, { tools: { shell: { mode: 'sandboxed' } } })],
    expect: (c) => {
      assert.equal(c.flat.get('tools.shell.mode'), 'sandboxed');
      assert.equal(c.src['tools.shell.mode'], 'developers');
    },
  },
  {
    name: 'scalar: equal priority, the Group whose name sorts first decides',
    groups: [doc('zeta', 10, { telemetry: { level: 'off' } }), doc('alpha', 10, { telemetry: { level: 'content' } })],
    expect: (c) => assert.equal(c.flat.get('telemetry.level'), 'content'),
  },
  {
    name: 'scalars are decided separately',
    groups: [
      doc('a', 20, { llm: { max_parallel_tools: 2 } }),
      doc('b', 10, { llm: { max_parallel_tools: 8, max_tool_result_kb: 128 } }),
    ],
    expect: (c) => {
      assert.equal(c.flat.get('llm.max_parallel_tools'), 2);
      assert.equal(c.flat.get('llm.max_tool_result_kb'), 128);
      assert.equal(c.src['llm.max_tool_result_kb'], 'b');
    },
  },
  {
    name: 'decision map: conflicting tools.rules, higher priority wins (Conflicting decision rules)',
    groups: [doc('developers', 20, { tools: { rules: { 'browser.*': 'allow' } } }), doc('standard', 10, { tools: { rules: { 'browser.*': 'approve' } } })],
    expect: (c) => {
      assert.deepEqual(c.flat.get('tools.rules'), { 'browser.*': 'allow' });
      assert.equal(c.src['tools.rules["browser.*"]'], 'developers');
    },
  },
  {
    name: 'decision map: a deny in any Group wins over a higher-priority allow',
    groups: [doc('developers', 20, { tools: { rules: { 'browser.*': 'allow' } } }), doc('standard', 10, { tools: { rules: { 'browser.*': 'deny' } } })],
    expect: (c) => {
      assert.deepEqual(c.flat.get('tools.rules'), { 'browser.*': 'deny' });
      assert.equal(c.src['tools.rules["browser.*"]'], 'standard');
    },
  },
  {
    name: 'decision map: entries from different Groups combine, three Groups in one pass',
    groups: [
      doc('a', 30, { tools: { rules: { 'mail.send': 'approve' }, risk: { external: 'allow' } } }),
      doc('b', 20, { tools: { rules: { 'browser.eval': 'deny', 'calendar.create': 'allow' } } }),
      doc('c', 10, { tools: { rules: { 'calendar.create': 'approve' }, risk: { destructive: 'approve' } }, unattended: { rules: { 'computer.*': 'deny' } } }),
    ],
    expect: (c) => {
      assert.deepEqual(c.flat.get('tools.rules'), { 'browser.eval': 'deny', 'calendar.create': 'allow', 'mail.send': 'approve' });
      assert.deepEqual(c.flat.get('tools.risk'), { destructive: 'approve', external: 'allow' });
      assert.deepEqual(c.flat.get('unattended.rules'), { 'computer.*': 'deny' });
    },
  },
  {
    name: 'lists: union across Groups; a deny in any Group prunes the allow list (Deny in any group wins)',
    groups: [
      doc('a', 20, { filesystem: { read: ['D:\\DEV\\**', 'D:\\Secret\\**'] }, network: { egress_allow: ['*.nuget.org'] } }),
      doc('b', 10, { filesystem: { read: ['%USERPROFILE%\\Documents\\**'], deny: ['D:\\Secret\\**'] }, network: { egress_allow: ['pypi.org'] } }),
    ],
    expect: (c) => {
      assert.deepEqual(c.flat.get('filesystem.read'), ['D:\\DEV\\**', '%USERPROFILE%\\Documents\\**']);
      assert.deepEqual(c.flat.get('filesystem.deny'), ['D:\\Secret\\**']);
      assert.deepEqual(c.flat.get('network.egress_allow'), ['*.nuget.org', 'pypi.org']);
    },
  },
  {
    name: 'whole map: llm.models from the highest-priority Group only (Model mapping)',
    groups: [
      doc('developers', 20, { llm: { models: { main: 'm-dev', vision: 'v-dev', small: 's-dev' } } }),
      doc('standard', 10, { llm: { models: { main: 'm-std', embed: 'e-std' } } }),
    ],
    expect: (c) => assert.deepEqual(c.flat.get('llm.models'), { main: 'm-dev', vision: 'v-dev', small: 's-dev' }),
  },
  {
    name: 'sparse: unset keys are absent (Unset key)',
    groups: [doc('a', 10, { tools: { shell: { mode: 'off' } } })],
    expect: (c) => {
      assert.deepEqual([...c.flat.keys()], ['tools.shell.mode']);
      assert.equal(c.flat.has('llm.budget.embed_daily_tokens'), false);
    },
  },
  {
    name: 'guardrail denies a domain that a Group allows, with a conflict',
    groups: [doc('developers', 20, { network: { egress_allow: ['*.example-filesharing.com', 'pypi.org'] } })],
    guardrail: { network: { egress_deny: ['*.example-filesharing.com'] } },
    expect: (c) => {
      assert.deepEqual(c.flat.get('network.egress_allow'), ['pypi.org']);
      assert.deepEqual(c.flat.get('network.egress_deny'), ['*.example-filesharing.com']);
      assert.equal(c.conflicts.length, 1);
      assert.equal(c.conflicts[0]!.group, 'developers');
    },
  },
  {
    name: 'guardrail bounds a number, with a conflict (Guardrail bounds a number)',
    groups: [doc('developers', 20, { tools: { shell: { limits: { memory_mb: 8192 } } } })],
    guardrail: { tools: { shell: { limits: { memory_mb: 4096 } } } },
    expect: (c) => {
      assert.equal(c.flat.get('tools.shell.limits.memory_mb'), 4096);
      assert.equal(c.src['tools.shell.limits.memory_mb'], 'guardrail');
      assert.deepEqual(c.conflicts.map((x) => [x.path, x.group_value, x.guardrail_value]), [['tools.shell.limits.memory_mb', 8192, 4096]]);
    },
  },
  {
    name: 'guardrail bounds a default that no Group sets (Guardrail bounds a default)',
    groups: [],
    guardrail: { llm: { budget: { tokens: 400000 } } },
    expect: (c) => {
      assert.equal(c.flat.get('llm.budget.tokens'), 400000);
      assert.equal(c.conflicts.length, 0);
    },
  },
  {
    name: 'guardrail cap above the value leaves it alone',
    groups: [doc('a', 10, { tools: { shell: { limits: { memory_mb: 2048 } } } })],
    guardrail: { tools: { shell: { limits: { memory_mb: 4096 } } } },
    expect: (c) => {
      assert.equal(c.flat.get('tools.shell.limits.memory_mb'), 2048);
      assert.equal(c.conflicts.length, 0);
    },
  },
  {
    name: 'guardrail forces approval over a Group allow (Guardrail forces approval)',
    groups: [doc('developers', 20, { tools: { rules: { 'browser.click': 'allow', 'mail.read': 'allow' } } })],
    guardrail: { tools: { rules: { 'browser.*': 'approve' } } },
    expect: (c) => {
      assert.deepEqual(c.flat.get('tools.rules'), { 'browser.*': 'approve', 'browser.click': 'approve', 'mail.read': 'allow' });
      assert.equal(c.conflicts.length, 1);
    },
  },
  {
    name: 'guardrail restricts a list (Guardrail restricts a list)',
    groups: [doc('developers', 20, { skills: { publish: ['shared', 'group', 'org'] } })],
    guardrail: { skills: { publish: ['shared'] } },
    expect: (c) => assert.deepEqual(c.flat.get('skills.publish'), ['shared']),
  },
  {
    name: 'guardrail max level on an ordered enumeration',
    groups: [doc('developers', 20, { tools: { shell: { mode: 'allow' } }, telemetry: { level: 'content' } })],
    guardrail: { tools: { shell: { mode: 'sandboxed' } }, telemetry: { level: 'metadata' } },
    expect: (c) => {
      assert.equal(c.flat.get('tools.shell.mode'), 'sandboxed');
      assert.equal(c.flat.get('telemetry.level'), 'metadata');
    },
  },
  {
    name: 'guardrail adds deny entries and makes a risk class stricter than its default',
    groups: [doc('a', 10, { filesystem: { deny: ['**\\.ssh\\**'] } })],
    guardrail: { filesystem: { deny: ['**\\*.kdbx'] }, tools: { risk: { external: 'deny' } } },
    expect: (c) => {
      assert.deepEqual(c.flat.get('filesystem.deny'), ['**\\.ssh\\**', '**\\*.kdbx']);
      assert.equal((c.flat.get('tools.risk') as Record<string, string>).external, 'deny');
    },
  },
  {
    name: 'guardrail forces model entries',
    groups: [doc('a', 10, { llm: { models: { main: 'x', small: 'y' } } })],
    guardrail: { llm: { models: { main: 'approved-main' } } },
    expect: (c) => assert.deepEqual(c.flat.get('llm.models'), { main: 'approved-main', small: 'y' }),
  },
  {
    name: 'cross-key rule over a combination of Groups (Scheduler concurrency above total)',
    groups: [doc('a', 20, { scheduler: { max_concurrent_runs: 4 } }), doc('b', 10, { llm: { max_concurrent_runs: 2 } })],
    expect: (c) => {
      assert.equal(c.errors.length, 1);
      assert.deepEqual(c.errors[0]!.keys, ['scheduler.max_concurrent_runs', 'llm.max_concurrent_runs']);
    },
  },
  {
    name: 'cross-key rule counts defaults (Defaults count in the rule)',
    groups: [doc('a', 20, { scheduler: { max_concurrent_runs: 4 } })],
    expect: (c) => assert.equal(c.errors.length, 1),
  },
  {
    name: 'cross-key rule is met',
    groups: [doc('a', 20, { scheduler: { max_concurrent_runs: 3 } })],
    expect: (c) => assert.equal(c.errors.length, 0),
  },
  {
    name: 'mcp-catalog entries are included with their source',
    groups: [],
    mcp: [{ id: 'jira-corp', version: '1.2.0', hash: 'sha256:abc', tools: { 'jira.search': 'read' } }],
    expect: (c) => {
      assert.equal((c.flat.get('mcp.servers') as unknown[]).length, 1);
      assert.equal(c.src['mcp.servers'], 'mcp-catalog');
    },
  },
];

for (const tc of cases) {
  test(`merge: ${tc.name}`, () => {
    const c = compile(reg, { groups: tc.groups, guardrail: tc.guardrail ? guard(tc.guardrail) : null, mcpServers: tc.mcp });
    tc.expect(c);
  });
}

test('merge: the result does not depend on the order the Groups are given in', () => {
  const g = [
    doc('x', 10, { tools: { rules: { 'a.*': 'approve' }, shell: { mode: 'off' } }, filesystem: { read: ['C:\\a\\**'] } }),
    doc('y', 10, { tools: { rules: { 'a.*': 'allow' }, shell: { mode: 'allow' } }, filesystem: { read: ['C:\\b\\**'] } }),
    doc('z', 5, { tools: { rules: { 'b.*': 'deny' } } }),
  ];
  const a = compile(reg, { groups: g });
  const b = compile(reg, { groups: [...g].reverse() });
  assert.deepEqual(a.policy, b.policy);
  assert.deepEqual(a.src, b.src);
});

test('validation: invalid values and unknown keys are rejected with the field (Invalid policy rejected, Unknown key rejected)', () => {
  const bad = reg.validate({ tools: { shell: { mode: 'maybe', colour: 'red' } } });
  assert.deepEqual(bad.errors.map((e) => e.path).sort(), ['tools.shell.colour', 'tools.shell.mode']);
  assert.match(bad.errors.find((e) => e.path === 'tools.shell.colour')!.message, /unknown/);
  assert.equal(reg.validate({ mcp: { servers: [] } }).errors[0]!.path, 'mcp.servers');
  assert.equal(reg.validate({ tools: { rules: { 'mail.send': 'sometimes' } } }).errors.length, 1);
  assert.equal(reg.validate({ tools: { risk: { exotic: 'allow' } } }).errors.length, 1);
});

test('validation: a guardrail key without a guardrail meaning is rejected (Guardrail key without meaning)', () => {
  const r = reg.validate({ filesystem: { read: ['C:\\**'] } }, { guardrail: true });
  assert.equal(r.errors[0]!.path, 'filesystem.read');
});

test('schema: the generated JSON Schema covers every key and rejects unknown ones', () => {
  const s = reg.jsonSchema() as any;
  assert.equal(s.additionalProperties, false);
  assert.deepEqual(s.properties.tools.properties.shell.properties.mode.enum, ['off', 'sandboxed', 'allow']);
  let leaves = 0;
  const walk = (n: any) => (n.properties && n.type === 'object' && !n['x-owner'] ? Object.values(n.properties).forEach(walk) : leaves++);
  walk(s);
  assert.equal(leaves, reg.keys.size);
  const g = reg.jsonSchema({ guardrail: true }) as any;
  assert.equal(g.properties.filesystem.properties.read, undefined);
});

test('schema: the client test fixture is the generated schema (no drift)', () => {
  const path = fileURLToPath(new URL('../../client/tests/fixtures/policy/schema.json', import.meta.url));
  assert.equal(readFileSync(path, 'utf8'), policyFixture(), 'run npm run policy:schema');
});

test('glob: ** crosses segments, * stays inside one, case-insensitive', () => {
  assert.equal(globMatch('**\\.ssh\\**', 'C:\\Users\\a\\.SSH\\id_rsa'), true);
  assert.equal(globMatch('D:\\DEV\\*', 'D:\\DEV\\x\\y'), false);
  assert.equal(globMatch('D:\\DEV\\**', 'd:\\dev\\x\\y'), true);
  assert.equal(globMatch('*.example.com', 'files.example.com'), true);
  assert.equal(globMatch('browser.*', 'browser.click'), true);
  assert.equal(globMatch('browser.*', 'mail.send'), false);
});
