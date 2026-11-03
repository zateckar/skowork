// Screenshots of every admin console view at given window widths, for checking the layout (improve-usability
// task 1.1): the built console against a seeded in-process control plane, signed in at the in-test identity
// provider, in headless Chrome or Edge, in the light and the dark colour scheme. Usage: node scripts/admin-screens.ts <out dir> [width ...]
import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { build } from 'vite';
import { Browser, findBrowser } from '../test/browser.ts';
import { fakeIdp } from '../test/idp.ts';
import { fakeUpstream, json, register, sleep, startHarness } from '../test/helpers.ts';

const [out = 'admin-screens', ...ws] = process.argv.slice(2);
const widths = ws.length ? ws.map(Number) : [1280, 1920];
const exe = findBrowser();
if (!exe) throw new Error('no Chrome or Edge installed');
mkdirSync(out, { recursive: true });

await build({ configFile: fileURLToPath(new URL('../admin/vite.config.ts', import.meta.url)), logLevel: 'error' });
// A model server for the LLM connections view.
const llm = await fakeUpstream();
const serve = llm.handler;
llm.handler = (path, body, res) =>
  path.endsWith('/models') ? json(res, 200, { data: ['acme-llm-primary', 'acme-llm-secondary', 'acme-llm-embedding', 'acme-llm-reranker'].map((id) => ({ id })) }) : serve(path, body, res);
const h = await startHarness((c) => (c.gateway.upstreams = [{ name: 'primary', baseUrl: llm.url, apiKey: 'sk-seed-0000abcd', maxConcurrent: 0 }]));
const idp = await fakeIdp(h);
h.cfg.identity.discoveryUrl = idp.url;
process.env.WORKBENCH_ADMIN_REDIRECT_URI = `${h.base}/admin/`;

// Seed: two Groups with members, a published policy, registered installations with heartbeats.
const admin = await h.token('admin1');
for (const [u, g] of [['alice', 'engineering'], ['bob', 'engineering'], ['carol', 'sales']] as const)
  await h.call('POST', '/api/v1/admin/identity/group-members', admin, { user_name: u, workbench_group: g });
await h.call('PUT', '/api/v1/admin/policy/groups/engineering/draft', admin, { document: { network: { egress_allow: ['*.example.com', 'github.com'] }, tools: { shell: { mode: 'sandboxed' }, computer: { mode: 'allow' }, rules: { 'computer.*': 'approve' } } }, priority: 10, reason: 'seed' });
await h.call('POST', '/api/v1/admin/policy/groups/engineering/publish', admin, { reason: 'seed' });
for (const u of ['alice', 'bob', 'carol']) {
  const token = await h.token(u);
  const { installation_id } = await register(h, token);
  const bundle = await h.call('GET', '/api/v1/policy/bundle', token);
  await h.call('POST', '/api/v1/fleet/heartbeat', token, { installation_id, status: 'idle', policy_version: String(bundle.body.version) });
}

const b = await Browser.launch(exe);
try {
  await b.goto(`${h.base}/admin/`);
  await b.click('sign-in');
  await b.waitFor(`!!document.querySelector('[data-testid="me"]')`);
  const views = await b.eval<string[]>(`[...document.querySelectorAll('[data-testid="nav"] a')].map((a) => a.dataset.testid.slice(4))`);
  for (const [width, scheme] of widths.flatMap((w) => [[w, 'light'], [w, 'dark']] as const)) {
    await b.send('Emulation.setDeviceMetricsOverride', { width, height: 900, deviceScaleFactor: 1, mobile: false });
    await b.send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: scheme }] });
    for (const v of views) {
      await b.click(`nav-${v}`);
      await b.waitFor(`!!document.querySelector('[data-testid="view-${v}"]')`);
      if (v === 'policy') await b.click('scope-engineering');
      if (v === 'connections') await b.click('conn-open-primary');
      if (v === 'models') await b.fill('models-group', 'engineering');
      await sleep(600);
      // Elements wider than the window or overlapping a sibling card are reported next to the picture.
      const issues = await b.eval<string[]>(`(() => {
        const r = [];
        if (document.documentElement.scrollWidth > innerWidth) r.push('page scrolls horizontally: ' + document.documentElement.scrollWidth);
        const cards = [...document.querySelectorAll('.card')];
        for (const c of cards) {
          const box = c.getBoundingClientRect();
          for (const el of c.querySelectorAll('*')) {
            const e = el.getBoundingClientRect();
            if (e.width && e.right > box.right + 1 && !el.closest('.scroll')) { r.push((c.dataset.testid || c.querySelector('h2')?.innerText) + ': ' + el.tagName + ' overflows by ' + Math.round(e.right - box.right)); break; }
          }
        }
        return r;
      })()`);
      const { height } = await b.eval<{ height: number }>(`({ height: document.documentElement.scrollHeight })`);
      const shot = await b.send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true, clip: { x: 0, y: 0, width, height: Math.min(height, 2400), scale: 1 } });
      const file = join(out, `${width}-${scheme}-${v}.png`);
      writeFileSync(file, Buffer.from(shot.data, 'base64'));
      console.log(`${file}${issues.length ? '\n  ' + issues.join('\n  ') : ''}`);
    }
  }
  if (b.errors.length) console.log('page errors:', b.errors);
} finally {
  await b.close();
  await idp.close();
  await h.close();
  await llm.close();
}
