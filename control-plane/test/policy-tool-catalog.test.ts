import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile, readdir } from 'node:fs/promises';
import { join } from 'node:path';
import { BUILT_IN_TOOL_DETAILS, BUILT_IN_TOOL_IDS } from '../src/policy/tool-catalog.ts';
import { KEYS } from '../src/policy/keys.ts';
import { Registry } from '../src/policy/schema.ts';
import { fakeMcp, startHarness } from './helpers.ts';

test('every registered policy key has an owner and explanation', () => {
  for (const key of KEYS) assert.ok(key.owner.trim() && key.description.trim(), `missing owner or explanation for ${key.path}`);
  assert.throws(() => new Registry(1, [{ path: 'example.missing', owner: 'client/example', description: '', type: { kind: 'boolean' }, merge: 'scalar' }]), /example\.missing.*description/);
});

function literalPolicyLookups(source: string): string[] {
  const pattern = /(?:policy_(?:str|string|bool|int|integer|list|number|double)|(?:env_|hooks_?)\.policy|policy)\s*\(\s*(?:p\s*,\s*)?"([a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+)"/g;
  return [...source.matchAll(pattern)].map((match) => match[1]!);
}

function unregisteredPolicyLookups(source: string, known: readonly string[]): string[] {
  return literalPolicyLookups(source).filter((key) => !known.some((path) => key === path || key.startsWith(`${path}.`)));
}

test('literal client policy lookups have a registered key or map owner', async () => {
  const root = new URL('../../client/src/', import.meta.url);
  const known = KEYS.map((key) => key.path);
  const misses: string[] = [];
  for (const entry of await readdir(root, { recursive: true, withFileTypes: true })) {
    if (!entry.isFile() || !/\.(cpp|h)$/.test(entry.name)) continue;
    const file = join(entry.parentPath, entry.name);
    for (const key of unregisteredPolicyLookups(await readFile(file, 'utf8'), known)) misses.push(`${key} (${file})`);
  }
  assert.deepEqual(misses, [], `unregistered literal client policy lookups: ${misses.join(', ')}`);
  assert.deepEqual(unregisteredPolicyLookups('policy_string("example.missing", "");', known), ['example.missing']);
});

function unregisteredSpecReferences(source: string, known: readonly string[]): string[] {
  // Scenarios may deliberately use invalid examples; only inspect the normative requirement text here.
  const requirements = source.replace(/^#### Scenario:[\s\S]*?(?=^### |(?![\s\S]))/gm, '');
  const paths = [...requirements.matchAll(/\bpolicy\.([a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+)/g)].map((match) => match[1]!);
  return paths.filter((key) => key !== 'mcp.servers' && !known.some((path) => key === path || key.startsWith(`${path}.`) || path.startsWith(`${key}.`)));
}

test('accepted specification policy references have a registered key or generated field', async () => {
  const root = new URL('../../openspec/specs/', import.meta.url);
  const known = KEYS.map((key) => key.path);
  const misses: string[] = [];
  for (const entry of await readdir(root, { recursive: true, withFileTypes: true })) {
    if (!entry.isFile() || entry.name !== 'spec.md') continue;
    const file = join(entry.parentPath, entry.name);
    for (const key of unregisteredSpecReferences(await readFile(file, 'utf8'), known)) misses.push(`${key} (${file})`);
  }
  assert.deepEqual(misses, [], `unregistered accepted policy references: ${misses.join(', ')}`);
  assert.deepEqual(unregisteredSpecReferences('### Requirement: New setting\nUse policy.example.missing.\n', known), ['example.missing']);
  assert.deepEqual(unregisteredSpecReferences('#### Scenario: Invalid example\nUse policy.example.invalid.\n', known), []);
});

function registeredToolIds(source: string): string[] {
  if (!source.includes('ToolDef')) return [];
  const direct = Array.from(source.matchAll(/\.id\s*=\s*"([a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+)"/g), (m) => m[1]!);
  // The current service factories construct ToolDef in an add lambda; RPC add() functions are not tool registrations.
  const toolFactory = /\bauto\s+add\s*=\s*\[[^\]]*\][^;]{0,500}?\bToolDef\s+\w+\s*;/.test(source);
  const factoryIds = toolFactory ? Array.from(source.matchAll(/\badd\("([a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+)"/g), (m) => m[1]!) : [];
  return [...direct, ...factoryIds];
}

test('built-in policy tool IDs agree with the current registration sites', async () => {
  const sourceRoot = new URL('../../client/src/', import.meta.url);
  const registered: string[] = [];
  for (const entry of await readdir(sourceRoot, { recursive: true, withFileTypes: true })) {
    if (!entry.isFile() || !entry.name.endsWith('.cpp')) continue;
    registered.push(...registeredToolIds(await readFile(join(entry.parentPath, entry.name), 'utf8')));
  }
  const runtime = await readFile(new URL('../../client/src/runtime/runtime.cpp', import.meta.url), 'utf8');
  registered.push(...Array.from(runtime.matchAll(/constexpr const char\* k(?:FindTools|AskUser|ReadAttachment|Delegate|UseSkill) = "([a-z_]+)"/g), (m) => m[1]!));
  assert.equal(new Set(BUILT_IN_TOOL_IDS).size, BUILT_IN_TOOL_IDS.length, 'duplicate catalog entry');
  assert.deepEqual([...BUILT_IN_TOOL_IDS].sort(), registered.sort());
});

test('registration inventory notices a new source file and ignores RPC add calls', () => {
  assert.deepEqual(registeredToolIds('ToolDef t; t.id = "newtool.action";'), ['newtool.action']);
  assert.deepEqual(registeredToolIds('ToolDef t; t.id = "newtool.v2.action3";'), ['newtool.v2.action3']);
  assert.deepEqual(registeredToolIds('auto add = [&](const char* id) { ToolDef t; }; add("newtool.action", x);'), ['newtool.action']);
  assert.deepEqual(registeredToolIds('ToolDef mentioned; rpc.add("settings.get", x);'), []);
  assert.equal(BUILT_IN_TOOL_IDS.includes('newtool.action' as typeof BUILT_IN_TOOL_IDS[number]), false);
});

test('local app, knowledge, scheduler, files and messaging metadata follows registered risks', async () => {
  for (const [file, prefix] of [
    ['apps/app_tools.cpp', 'apps.'],
    ['knowledge/knowledge_tools.cpp', 'knowledge.'],
    ['scheduler/task_tools.cpp', 'tasks.'],
    ['tools/files.cpp', 'files.'],
    ['messaging/messaging_tools.cpp', 'messages.'],
  ] as const) {
    const source = await readFile(new URL(`../../client/src/${file}`, import.meta.url), 'utf8');
    const registrations = [...source.matchAll(/\b(\w+)\.id\s*=\s*"([a-z]+\.[a-z_]+)";(?:(?!\b\1\.id\s*=)[\s\S])*?\b\1\.risk\s*=\s*(?:runtime::)?Risk::([a-z_]+);/g)]
      .filter((m) => m[2]!.startsWith(prefix));
    assert.deepEqual(registrations.map((m) => m[2]).sort(), BUILT_IN_TOOL_IDS.filter((id) => id.startsWith(prefix)).sort());
    for (const [, , id, risk] of registrations) {
      const detail = BUILT_IN_TOOL_DETAILS[id!];
      assert.ok(detail && detail.owner && detail.description && detail.prerequisite && detail.caveat, `missing metadata for ${id}`);
      assert.equal(detail.declared_risk, risk, `${id} risk differs from registration`);
    }
  }
});

test('every built-in has metadata and runtime core risks match registrations', async () => {
  assert.deepEqual(Object.keys(BUILT_IN_TOOL_DETAILS).sort(), [...BUILT_IN_TOOL_IDS].sort());
  for (const [id, detail] of Object.entries(BUILT_IN_TOOL_DETAILS)) {
    assert.ok(detail.owner && detail.description && detail.prerequisite && detail.caveat && detail.declared_risk, `incomplete metadata for ${id}`);
  }
  const source = await readFile(new URL('../../client/src/runtime/runtime.cpp', import.meta.url), 'utf8');
  for (const [, constant, id] of source.matchAll(/constexpr const char\* (k(?:FindTools|AskUser|ReadAttachment|Delegate|UseSkill)) = "([a-z_]+)"/g)) {
    const registration = new RegExp(`\\.id = ${constant};[\\s\\S]*?\\.risk = Risk::([a-z_]+);`).exec(source);
    assert.ok(registration, `missing registration for ${id}`);
    assert.equal(BUILT_IN_TOOL_DETAILS[id!]!.declared_risk, registration[1]);
  }
  for (const [file, id] of [['tools/web.cpp', 'web.fetch'], ['tools/image.cpp', 'image.generate']] as const) {
    const tool = await readFile(new URL(`../../client/src/${file}`, import.meta.url), 'utf8');
    const risk = /\.risk = (?:runtime::)?Risk::([a-z_]+);/.exec(tool);
    assert.ok(risk);
    assert.equal(BUILT_IN_TOOL_DETAILS[id]!.declared_risk, risk[1]);
  }
  const shell = await readFile(new URL('../../client/src/tools/shell.cpp', import.meta.url), 'utf8');
  const rules = await readFile(new URL('../../client/src/runtime/tools.cpp', import.meta.url), 'utf8');
  assert.match(shell, /t\.risk_of\s*=/);
  assert.match(rules, /if\s*\(tool\.risk_of\)\s*return\s+Risk::destructive/);
  assert.equal(BUILT_IN_TOOL_DETAILS['shell.run']!.declared_risk, 'destructive');
});

test('built-in owners name their OpenSpec capabilities', async () => {
  const specs: Record<string, string> = {
    'client/agent-runtime': 'add-agent-runtime',
    'client/local-apps': 'add-local-apps',
    'client/knowledge-graph': 'add-knowledge-graph',
    'client/scheduler': 'add-scheduled-and-remote-tasks',
    'client/core-tools': 'add-agent-runtime',
    'client/mail-calendar': 'add-mail-calendar',
    'client/browser-use': 'add-agent-runtime',
    'client/computer-use': 'add-computer-use',
    'client/user-messaging': 'add-user-messaging',
    'client/agent-skills': 'add-agent-created-skills',
  };
  for (const owner of new Set(Object.values(BUILT_IN_TOOL_DETAILS).map((detail) => detail.owner))) {
    assert.ok(specs[owner], `no owning capability spec for ${owner}`);
    // Once an in-flight capability is archived, its accepted spec replaces the delta at this path.
    const candidates = [`openspec/specs/${owner}/spec.md`, `openspec/changes/${specs[owner]}/specs/${owner}/spec.md`,
      `openspec/changes/improve-skowork-and-knowledge-control/specs/${owner}/spec.md`];
    const contents = (await Promise.all(candidates.map((path) => readFile(new URL(`../../${path}`, import.meta.url), 'utf8').catch(() => '')))).join('\n');
    assert.match(contents, /## (?:ADDED |MODIFIED )?Requirements/);
    for (const [id, detail] of Object.entries(BUILT_IN_TOOL_DETAILS)) {
      if (detail.owner === owner) assert.ok(contents.includes(id), `${id} is missing from its owning capability specifications`);
    }
  }
});

test('computer, browser, mail and calendar metadata follows registered risks', async () => {
  for (const [file, prefixes] of [
    ['computer/service.cpp', ['computer.']],
    ['browser/service.cpp', ['browser.']],
    ['mail/mail_tools.cpp', ['mail.', 'calendar.']],
  ] as const) {
    const source = await readFile(new URL(`../../client/src/${file}`, import.meta.url), 'utf8');
    const registrations = [...source.matchAll(/\badd\("([a-z]+\.[a-z_]+)"\s*,(?:(?!\badd\(")[\s\S])*?\bRisk::([a-z_]+)/g)]
      .filter((m) => prefixes.some((prefix) => m[1]!.startsWith(prefix)));
    assert.deepEqual(registrations.map((m) => m[1]).sort(), BUILT_IN_TOOL_IDS.filter((id) => prefixes.some((prefix) => id.startsWith(prefix))).sort());
    for (const [, id, risk] of registrations) {
      const detail = BUILT_IN_TOOL_DETAILS[id!];
      assert.ok(detail && detail.owner && detail.description && detail.prerequisite && detail.caveat, `missing metadata for ${id}`);
      assert.equal(detail.declared_risk, risk, `${id} risk differs from registration`);
    }
  }
});

test('policy schema exposes unset computer tools and accepts custom patterns', async () => {
  const h = await startHarness();
  try {
    const admin = await h.token('admin1');
    const schema = await h.call('GET', '/api/v1/admin/policy/schema?keys=true', admin);
    assert.equal(schema.status, 200);
    assert.equal(schema.body.tool_catalog_schema_version, 1);
    const key = (path: string) => schema.body.keys.find((entry: { path: string }) => entry.path === path);
    assert.match(key('tools.computer.mode').description, /click, type and screenshot/);
    assert.match(key('unattended.rules').description, /unattended runs/);
    assert.match(key('knowledge.sharing.groups').description, /empty list disables sharing/i);
    assert.deepEqual(schema.body.generated_fields, [{ path: 'mcp.servers', owner: 'control-plane/mcp-catalog',
      description: 'Server entries are generated from the MCP catalog for the user; manage them in MCP Servers.', editable: false }]);
    assert.ok(!schema.body.keys.some((key: { path: string }) => key.path === 'mcp.servers'));
    assert.match(schema.body.tool_catalog_revision, /^[a-f0-9]{64}$/);
    assert.ok(schema.body.built_in_tools.includes('computer.click'));
    assert.deepEqual(schema.body.tool_details['computer.click'].decision_paths, ['tools.rules["computer.click"]', 'unattended.rules["computer.click"]']);
    const computer = schema.body.built_in_tools.filter((id: string) => id.startsWith('computer.'));
    assert.equal(computer.length, 9);
    for (const id of computer) {
      const detail = schema.body.tool_details[id];
      assert.equal(detail.owner, 'client/computer-use');
      assert.ok(detail.description && detail.prerequisite && detail.caveat && detail.declared_risk);
    }
    assert.equal(schema.body.tool_details['apps.delete'].declared_risk, 'destructive');
    assert.match(schema.body.tool_details['knowledge.remember'].description, /Remember/);
    const saved = await h.call('PUT', '/api/v1/admin/policy/groups/engineering/draft', admin, {
      document: { tools: { rules: { 'computer.click': 'approve', 'mcp:custom/*': 'deny' } } }, priority: 10, reason: 'test',
    });
    assert.equal(saved.status, 200);
    const generated = await h.call('PUT', '/api/v1/admin/policy/groups/engineering/draft', admin, {
      document: { mcp: { servers: [] } }, reason: 'test',
    });
    assert.equal(generated.status, 400);
  } finally {
    await h.close();
  }
});

test('discovered MCP policy tools are current and visible only to platform administrators', async () => {
  const mcp = await fakeMcp();
  mcp.tools = [{ name: 'find', description: 'Find an issue' }, { name: 'remove', description: 'Remove an issue' }];
  const h = await startHarness((cfg) => { cfg.mcpCatalog.allowInsecureHttp = true; });
  try {
    const admin = await h.token('admin1');
    for (const [name, role] of [['owner1', 'group-owner'], ['auditor1', 'security-auditor']]) {
      assert.equal((await h.call('POST', '/api/v1/admin/identity/admin-roles', admin, { user_name: name, role })).status, 200);
    }
    const create = await h.call('POST', '/api/v1/admin/mcp/servers', admin, {
      id: 'issues', name: 'Issues', owner: 'it', kind: 'remote', url: mcp.url, auth: 'none', groups: ['engineering'], risk: { find: 'read' },
    });
    assert.equal(create.status, 200, JSON.stringify(create.body));
    let schema = await h.call('GET', '/api/v1/admin/policy/schema?keys=true', admin);
    const revision = schema.body.tool_catalog_revision;
    assert.equal((await h.call('GET', '/api/v1/admin/policy/schema?keys=true', admin)).body.tool_catalog_revision, revision);
    assert.deepEqual(schema.body.discovered_tools, ['mcp:issues/find', 'mcp:issues/remove']);
    assert.equal(schema.body.tool_details['mcp:issues/find'].declared_risk, 'read');
    assert.deepEqual(schema.body.tool_details['mcp:issues/find'].decision_paths, ['tools.rules["mcp:issues/find"]', 'unattended.rules["mcp:issues/find"]']);
    assert.equal(schema.body.tool_details['mcp:issues/remove'].declared_risk, 'external');
    assert.match(schema.body.tool_details['mcp:issues/find'].description, /Find an issue/);
    let restrictedRevision = '';
    for (const name of ['owner1', 'auditor1']) {
      const response = await h.call('GET', '/api/v1/admin/policy/schema?keys=true', await h.token(name));
      assert.equal(response.status, 200);
      assert.deepEqual(response.body.discovered_tools, []);
      assert.doesNotMatch(JSON.stringify(response.body), /mcp:issues|Find an issue/);
      assert.notEqual(response.body.tool_catalog_revision, revision);
      if (restrictedRevision) assert.equal(response.body.tool_catalog_revision, restrictedRevision);
      restrictedRevision = response.body.tool_catalog_revision;
    }
    assert.equal((await h.call('GET', '/api/v1/admin/policy/schema?keys=true', await h.token('dev1'))).status, 403);

    mcp.tools = [{ name: 'new', description: 'Newly discovered' }];
    assert.equal((await h.call('POST', '/api/v1/admin/mcp/servers/issues/discover', admin)).status, 200);
    schema = await h.call('GET', '/api/v1/admin/policy/schema?keys=true', admin);
    assert.notEqual(schema.body.tool_catalog_revision, revision);
    assert.equal((await h.call('GET', '/api/v1/admin/policy/schema?keys=true', await h.token('owner1'))).body.tool_catalog_revision, restrictedRevision);
    assert.deepEqual(schema.body.discovered_tools, ['mcp:issues/new']);
    assert.match(schema.body.tool_details['mcp:issues/new'].caveat, /pending/);
  } finally {
    await h.close();
    await mcp.close();
  }
});
