import { test } from 'node:test';
import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';
import { build } from 'vite';
import { Browser, findBrowser } from './browser.ts';
import { fakeIdp } from './idp.ts';
import { fakeMcp, startHarness } from './helpers.ts';

const browser = findBrowser();
test('policy editor finds an unset computer tool and preserves a custom rule', { skip: !browser && 'no Chrome or Edge installed' }, async () => {
  await build({ configFile: fileURLToPath(new URL('../admin/vite.config.ts', import.meta.url)), logLevel: 'error' });
  const mcp = await fakeMcp();
  mcp.tools = [{ name: 'lookup', description: 'Look up a ticket' }];
  const h = await startHarness((cfg) => { cfg.mcpCatalog.allowInsecureHttp = true; });
  const idp = await fakeIdp(h);
  h.cfg.identity.discoveryUrl = idp.url;
  process.env.SKOWORK_ADMIN_REDIRECT_URI = `${h.base}/admin/`;
  let b: Browser | undefined;
  try {
    const admin = await h.token('admin1');
    const created = await h.call('POST', '/api/v1/admin/mcp/servers', admin, {
      id: 'tickets', name: 'Tickets', owner: 'it', kind: 'remote', url: mcp.url, auth: 'none', groups: ['engineering'], risk: { lookup: 'read' },
    });
    assert.equal(created.status, 200, JSON.stringify(created.body));
    b = await Browser.launch(browser!);
    idp.next.user = 'admin1';
    await b.goto(`${h.base}/admin/`);
    await b.click('sign-in');
    await b.waitFor(`!!document.querySelector('[data-testid="me"]')`);
    await b.click('nav-policy');
    await b.fill('policy-form-filter', 'mcp.servers');
    await b.waitFor(`document.querySelector('[data-testid="generated-mcp.servers"]')?.textContent.includes('control-plane/mcp-catalog')`);
    assert.equal(await b.eval(`document.querySelectorAll('[data-testid="generated-mcp.servers"] input, [data-testid="generated-mcp.servers"] select').length`), 0);
    await b.fill('policy-form-filter', 'client/computer-use');
    await b.waitFor(`document.querySelector('[data-testid="key-row-tools.computer.mode"]')?.textContent.includes('The highest-priority group')`);
    await b.waitFor(`document.querySelector('[data-testid="key-row-tools.computer.mode"]')?.textContent.includes('guardrail limits the permitted level')`);
    await b.fill('policy-form-filter', 'enable computer-use tools such as click');
    await b.waitFor(`document.querySelector('[data-testid="key-row-tools.computer.mode"]')?.textContent.includes('Enable computer-use tools such as click')`);
    await b.fill('policy-form-filter', 'daily per-user token budget');
    await b.waitFor(`document.querySelector('[data-testid="key-row-knowledge.embedding_daily_tokens"]')?.textContent.includes('background knowledge embeddings')`);
    await b.fill('policy-form-filter', 'automation and its history');
    await b.waitFor(`document.querySelector('[data-testid="key-tools.rules"]')?.textContent.includes('tasks.delete')`);
    await b.waitFor(`document.querySelector('[data-testid="key-tools.rules"]')?.textContent.includes('Owner: client/scheduler')`);
    await b.fill('policy-form-filter', 'mcp:tickets/lookup');
    await b.waitFor(`document.querySelector('[data-testid="key-tools.rules"]')?.textContent.includes('Look up a ticket')`);
    await b.fill('key-tools.rules-mcp:tickets/lookup', 'approve');
    await b.fill('policy-form-filter', 'computer.click');
    await b.waitFor(`document.querySelector('[data-testid="key-tools.rules"]')?.textContent.includes('Computer mode must allow use.')`);
    await b.waitFor(`document.querySelector('[data-testid="key-tools.rules"]')?.textContent.includes('policy.unattended.rules["computer.click"]')`);
    await b.fill('key-tools.rules-computer.click', 'approve');
    await b.fill('key-tools.rules-new', 'mcp:custom/*');
    assert.equal(await b.waitFor<boolean>(`document.querySelector('[data-testid="key-tools.rules-add"]')?.disabled`), true);
    await b.fill('key-tools.rules-new-decision', 'deny');
    await b.click('key-tools.rules-add');
    await b.fill('policy-form-filter', 'mcp:custom');
    await b.waitFor(`!!document.querySelector('[data-testid="key-tools.rules-mcp:custom/*"]')`);
    await b.click('draft-mode-json');
    let doc = JSON.parse(await b.waitFor<string>(`document.querySelector('[data-testid="draft-text"]')?.value`));
    assert.deepEqual(doc.tools.rules, { 'mcp:tickets/lookup': 'approve', 'computer.click': 'approve', 'mcp:custom/*': 'deny' });
    await b.click('draft-mode-form');
    await b.fill('policy-form-filter', 'computer.click');
    await b.fill('key-tools.rules-computer.click', '');
    await b.click('draft-mode-json');
    doc = JSON.parse(await b.waitFor<string>(`document.querySelector('[data-testid="draft-text"]')?.value`));
    assert.deepEqual(doc.tools.rules, { 'mcp:tickets/lookup': 'approve', 'mcp:custom/*': 'deny' });
    const group = await h.call('PUT', '/api/v1/admin/policy/groups/engineering/draft', admin, {
      document: { tools: { computer: { mode: 'allow' }, rules: { 'computer.click': 'allow' } } }, reason: 'simulation test',
    });
    assert.equal(group.status, 200, JSON.stringify(group.body));
    const guardrail = await h.call('PUT', '/api/v1/admin/policy/guardrail/draft', admin, {
      document: { tools: { rules: { 'computer.click': 'deny' } } }, reason: 'simulation test',
    });
    assert.equal(guardrail.status, 200, JSON.stringify(guardrail.body));
    await b.fill('sim-groups-input', 'engineering');
    await b.fill('sim-tool-input', 'computer.click');
    await b.fill('sim-risk', 'external');
    await b.submit('simulate');
    await b.waitFor(`document.querySelector('[data-testid="simulation-tool-decision"]')?.textContent.includes('tools.computer.mode')`);
    const published = await b.text('simulation-tool-decision');
    assert.match(published, /Example tool decision: deny/);
    assert.match(published, /published policy/);
    assert.match(published, /switched off by policy/);
    await b.submit('simulate', { drafts: true, unattended: true });
    await b.waitFor(`document.querySelector('[data-testid="simulation-tool-decision"]')?.textContent.includes('including saved drafts')`);
    const decision = await b.text('simulation-tool-decision');
    assert.match(decision, /computer\.click/);
    assert.match(decision, /assumed risk: external/);
    assert.match(decision, /unattended/);
    assert.match(decision, /including saved drafts/);
    assert.match(decision, /guardrail/);
    assert.match(decision, /actual action and target may change/);
    assert.deepEqual(b.errors, []);
  } finally {
    await b?.close();
    await idp.close();
    await h.close();
    await mcp.close();
    delete process.env.SKOWORK_ADMIN_REDIRECT_URI;
  }
});
