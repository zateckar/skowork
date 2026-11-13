// control-plane/llm-gateway, against an in-test upstream (faults the real API cannot produce on demand).
// The same behaviour against the real organization LLM API is covered by test-real/ (local only).
import { after, afterEach, before, describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import type { Config } from '../src/config.ts';
import { fakeUpstream, json, startHarness, waitUntil, type FakeUpstream, type Harness } from './helpers.ts';

const KEY_A = 'upstream-key-a-0123456789';
const KEY_B = 'upstream-key-b-9876543210';

describe('gateway', () => {
  let a: FakeUpstream;
  let b: FakeUpstream;
  let h: Harness;
  let token: string;
  const defaultHandler = { a: null as FakeUpstream['handler'] | null, b: null as FakeUpstream['handler'] | null };

  before(async () => {
    a = await fakeUpstream();
    b = await fakeUpstream();
    defaultHandler.a = a.handler;
    defaultHandler.b = b.handler;
    h = await startHarness((c) => {
      c.gateway.upstreams = [
        { name: 'a', baseUrl: a.url, apiKey: KEY_A, maxConcurrent: 0 },
        { name: 'b', baseUrl: b.url, apiKey: KEY_B, maxConcurrent: 0 },
      ];
      c.gateway.healthCheckIntervalMs = 100;
    });
    token = await h.token('dev1');
  });
  afterEach(() => {
    a.handler = defaultHandler.a!;
    b.handler = defaultHandler.b!;
    a.requests.length = 0;
    b.requests.length = 0;
    for (const be of h.cp.gateway.pool.backends) be.healthy = true;
  });
  after(async () => {
    await h.close();
    await a.close();
    await b.close();
  });

  const chat = (t: string, body: Record<string, unknown> = {}) =>
    h.call('POST', '/llm/v1/chat/completions', t, { model: 'main', messages: [{ role: 'user', content: 'hi' }], ...body });
  const sent = () => [...a.requests, ...b.requests].filter((r) => !r.path.endsWith('/models'));

  it('refuses requests without a valid token before any upstream call', async () => {
    assert.equal((await h.call('POST', '/llm/v1/chat/completions', null, { model: 'main' })).status, 401);
    assert.equal((await h.call('GET', '/llm/v1/models', 'bad')).status, 401);
    assert.equal(sent().length, 0);
  });

  it('lists the roles with their capabilities', async () => {
    const r = await h.call('GET', '/llm/v1/models', token);
    assert.deepEqual(r.body.data.map((m: any) => m.id), ['main', 'vision', 'small', 'embed', 'rerank', 'image']);
    assert.equal(r.body.data.find((m: any) => m.id === 'image').kind, 'image');
    const main = r.body.data[0];
    assert.equal(main.kind, 'chat');
    assert.equal(main.tools, true);
    assert.equal(r.body.data[3].dimension, 4096);
    assert.equal(r.body.data[3].model, 'acme-llm-embedding');
  });

  it('forwards chat by role to the mapped model with the gateway credential only', async () => {
    const r = await chat(token);
    assert.equal(r.status, 200);
    assert.equal(r.body.choices[0].message.content, 'Hello');
    const req = sent()[0]!;
    assert.equal(req.body.model, 'acme-llm-primary');
    assert.match(String(req.headers.authorization), /^Bearer upstream-key-[ab]/);
    assert.ok(!String(req.headers.authorization).includes(token.slice(0, 20)));
    await h.call('POST', '/llm/v1/chat/completions', token, { model: 'small', messages: [] });
    assert.equal(sent()[1]!.body.model, 'acme-llm-secondary');
  });

  it('passes a stream through and requests usage reporting', async () => {
    const r = await chat(token, { stream: true });
    assert.equal(r.status, 200);
    assert.match(r.headers.get('content-type') ?? '', /text\/event-stream/);
    assert.match(String(r.body), /Hello[\s\S]*\[DONE\]/);
    assert.equal(sent()[0]!.body.stream_options.include_usage, true);
    await waitUntil(() => {
      const u = h.cp.db.prepare("SELECT total_tokens, outcome FROM llm_usage WHERE endpoint = 'chat' ORDER BY id DESC LIMIT 1").get() as any;
      return u?.total_tokens === 12 && u.outcome === 'ok';
    });
  });

  it('records a stream cut after the first chunk as truncated', async () => {
    const cut: FakeUpstream['handler'] = (_p, _b, res) => {
      res.writeHead(200, { 'content-type': 'text/event-stream' });
      res.write(`data: ${JSON.stringify({ choices: [{ delta: { content: 'par' } }] })}\n\n`);
      setTimeout(() => res.socket?.destroy(), 20);
    };
    a.handler = cut;
    b.handler = cut;
    const r = await chat(token, { stream: true });
    assert.equal(r.status, 200);
    assert.doesNotMatch(String(r.body), /\[DONE\]/);
    await waitUntil(() => (h.cp.db.prepare("SELECT outcome FROM llm_usage WHERE endpoint = 'chat' ORDER BY id DESC LIMIT 1").get() as any)?.outcome === 'truncated');
  });

  it('fails over when a backend answers 500 or dies before the first byte', async () => {
    a.handler = (_p, _b, res) => json(res, 500, { error: 'boom at 10.1.2.3' });
    for (let i = 0; i < 4; i++) assert.equal((await chat(token)).status, 200);
    assert.equal(b.requests.filter((r) => r.path.endsWith('/chat/completions')).length, 4);
    a.handler = (_p, _b, res) => res.socket?.destroy();
    for (const be of h.cp.gateway.pool.backends) be.healthy = true;
    const r = await chat(await h.token('dev2'), { stream: true });
    assert.equal(r.status, 200);
    assert.match(String(r.body), /\[DONE\]/);
  });

  it('sends nothing to an unhealthy backend and returns to it when it recovers', async () => {
    a.handler = (_p, _b, res) => res.socket?.destroy();
    await chat(token);
    const backendA = h.cp.gateway.pool.backends[0]!;
    assert.equal(backendA.healthy, false);
    a.requests.length = 0;
    for (let i = 0; i < 3; i++) assert.equal((await chat(await h.token('dev3'))).status, 200);
    assert.equal(a.requests.filter((r) => r.path.endsWith('/chat/completions')).length, 0);
    a.handler = defaultHandler.a!;
    await waitUntil(() => backendA.healthy, 2000);
  });

  it('answers MODEL_UNAVAILABLE without upstream details when every backend is down', async () => {
    const down: FakeUpstream['handler'] = (_p, _b, res) => json(res, 503, { error: `upstream ${a.url} key ${KEY_A}` });
    a.handler = down;
    b.handler = down;
    const r = await chat(token);
    assert.equal(r.status, 503);
    assert.equal(r.body.error.code, 'MODEL_UNAVAILABLE');
    const text = JSON.stringify(r.body) + JSON.stringify([...r.headers]);
    for (const secret of [KEY_A, KEY_B, '127.0.0.1', a.url]) assert.ok(!text.includes(secret), `leaked ${secret}`);
  });

  it('maps an upstream 429 to RATE_LIMIT with a retry delay, and a context error to CONTEXT_LENGTH', async () => {
    const busy: FakeUpstream['handler'] = (_p, _b, res) => {
      res.writeHead(429, { 'retry-after': '7', 'content-type': 'application/json' });
      res.end('{}');
    };
    a.handler = busy;
    b.handler = busy;
    const r = await chat(token);
    assert.equal(r.status, 429);
    assert.equal(r.body.error.code, 'RATE_LIMIT');
    assert.equal(r.body.error.retry_after_s, 7);
    const ctx: FakeUpstream['handler'] = (_p, _b, res) => json(res, 400, { error: { message: "This model's maximum context length is 32768 tokens" } });
    a.handler = ctx;
    b.handler = ctx;
    const c = await chat(token);
    assert.equal(c.body.error.code, 'CONTEXT_LENGTH');
    assert.doesNotMatch(JSON.stringify(c.body), /32768/);
  });

  it('refuses a request larger than the context window before any upstream call', async () => {
    const r = await chat(token, { messages: [{ role: 'user', content: 'x'.repeat(800_000) }] });  // well over main's 131072 tokens
    assert.equal(r.status, 400);
    assert.equal(r.body.error.code, 'CONTEXT_LENGTH');
    assert.equal(sent().length, 0);
  });

  it('refuses a role that does not fit the endpoint or is unknown', async () => {
    assert.equal((await h.call('POST', '/llm/v1/embeddings', token, { model: 'main', input: ['x'] })).body.error.code, 'INVALID_REQUEST');
    assert.equal((await h.call('POST', '/llm/v1/chat/completions', token, { model: 'gpt-4', messages: [] })).body.error.code, 'MODEL_UNAVAILABLE');
    assert.equal(sent().length, 0);
  });

  it('returns embeddings with the upstream model and dimension, and rerank scores in input order', async () => {
    const e = await h.call('POST', '/llm/v1/embeddings', token, { model: 'embed', input: Array.from({ length: 16 }, (_, i) => `chunk ${i}`) });
    assert.equal(e.status, 200);
    assert.equal(e.body.data.length, 16);
    assert.equal(e.body.model, 'acme-llm-embedding');
    assert.equal(e.body.dimension, 4);
    const rr = await h.call('POST', '/llm/v1/rerank', token, { model: 'rerank', query: 'q', documents: Array.from({ length: 30 }, (_, i) => `p${i}`) });
    assert.equal(rr.body.results.length, 30);
    assert.deepEqual(rr.body.results.map((x: any) => x.index), [...Array(30).keys()]);
  });

  it('keeps embedding usage out of the chat budget, and rejects with QUOTA_EXCEEDED and a reset time when used up', async () => {
    const u = await h.token('budget1');
    await h.call('POST', '/llm/v1/embeddings', u, { model: 'embed', input: ['a'] });
    const chatUsed = h.cp.db.prepare("SELECT COALESCE(SUM(total_tokens),0) AS t FROM llm_usage WHERE user_name = 'budget1' AND bucket = 'chat'").get() as any;
    assert.equal(chatUsed.t, 0);
    h.cp.db.prepare(`INSERT INTO llm_usage (at, subject, user_name, endpoint, role, bucket, total_tokens, outcome, duration_ms)
      VALUES (?, 'sub-budget1', 'budget1', 'chat', 'main', 'chat', 2000000, 'ok', 1)`).run(new Date().toISOString());
    const r = await chat(u);
    assert.equal(r.status, 429);
    assert.equal(r.body.error.code, 'QUOTA_EXCEEDED');
    assert.ok(Date.parse(r.body.error.reset_at) > Date.now());
    // Embeddings have their own budget and still work.
    assert.equal((await h.call('POST', '/llm/v1/embeddings', u, { model: 'embed', input: ['a'] })).status, 200);
  });

  it('rejects requests over the per-minute limit with a retry delay', async () => {
    const u = await h.token('rpm1');
    let limited: any = null;
    for (let i = 0; i < 65 && !limited; i++) {
      const r = await chat(u);
      if (r.status === 429) limited = r.body;
    }
    assert.equal(limited?.error.code, 'RATE_LIMIT');
    assert.ok(limited.error.retry_after_s >= 1);
  });

  it('audits every request with metadata and no content at the default level', async () => {
    await chat(await h.token('audited1'), { messages: [{ role: 'user', content: 'secret project name' }] });
    const row = h.cp.db.prepare("SELECT * FROM llm_usage WHERE user_name = 'audited1'").get() as any;
    assert.equal(row.role, 'main');
    assert.equal(row.upstream_model, 'acme-llm-primary');
    assert.equal(row.content, null);
  });

  it('records the Run identifier, the installation and the Groups in the audit', async () => {
    const t = await h.token('audited2');
    await h.call('POST', '/llm/v1/chat/completions', t, { model: 'main', messages: [{ role: 'user', content: 'hi' }] },
      { 'x-skowork-run': 'run_abc', 'x-skowork-installation': 'inst-42' });
    const row = h.cp.db.prepare("SELECT * FROM llm_usage WHERE user_name = 'audited2' AND endpoint = 'chat'").get() as any;
    assert.equal(row.run_id, 'run_abc');
    assert.equal(row.installation_id, 'inst-42');
    assert.ok(Array.isArray(JSON.parse(row.groups)));
    assert.equal(row.total_tokens, 12);
    assert.equal(row.outcome, 'ok');
    assert.ok(row.duration_ms >= 0);
  });

  it('rejects requests over the per-user concurrency limit with a retry delay', async () => {
    a.handler = delayed(defaultHandler.a!, 300);
    b.handler = delayed(defaultHandler.b!, 300);
    const u = await h.token('burst1');
    const results = await Promise.all(Array.from({ length: 7 }, () => chat(u)));
    const limited = results.filter((r) => r.status === 429);
    assert.equal(results.filter((r) => r.status === 200).length, h.cfg.gateway.concurrentRequests);
    assert.equal(limited.length, 7 - h.cfg.gateway.concurrentRequests);
    for (const r of limited) {
      assert.equal(r.body.error.code, 'RATE_LIMIT');
      assert.ok(r.body.error.retry_after_s >= 1);
    }
  });

  it('rejects with RATE_LIMIT when every healthy backend is at its capacity', async () => {
    for (const be of h.cp.gateway.pool.backends) (be as any).maxConcurrent = 1;
    a.handler = delayed(defaultHandler.a!, 300);
    b.handler = delayed(defaultHandler.b!, 300);
    try {
      const results = await Promise.all(['cap1', 'cap2', 'cap3'].map(async (u) => chat(await h.token(u))));
      assert.equal(results.filter((r) => r.status === 200).length, 2);
      const limited = results.find((r) => r.status === 429)!;
      assert.equal(limited.body.error.code, 'RATE_LIMIT');
      assert.ok(limited.body.error.retry_after_s >= 1);
    } finally {
      for (const be of h.cp.gateway.pool.backends) (be as any).maxConcurrent = 0;
    }
  });

  it('counts rerank usage from billed units against the embed budget only', async () => {
    const billed: FakeUpstream['handler'] = (_p, body, res) =>
      json(res, 200, { results: body.documents.map((_: unknown, i: number) => ({ index: i, relevance_score: 0.5 })), meta: { billed_units: { input_tokens: 800 } } });
    a.handler = billed;
    b.handler = billed;
    const u = await h.token('rerank1');
    assert.equal((await h.call('POST', '/llm/v1/rerank', u, { model: 'rerank', query: 'q', documents: ['a', 'b'] })).status, 200);
    const rows = h.cp.db.prepare("SELECT bucket, total_tokens, content FROM llm_usage WHERE user_name = 'rerank1' AND endpoint = 'rerank'").all() as any[];
    assert.deepEqual(rows.map((r) => [r.bucket, r.total_tokens, r.content]), [['embed', 800, null]]);
  });

  const image = (t: string, body: Record<string, unknown> = {}) =>
    h.call('POST', '/llm/v1/images/generations', t, { model: 'image', prompt: 'a red car', ...body });

  it('generates images with the image role and counts them against the image budget only', async () => {
    const u = await h.token('image1');
    const r = await image(u, { n: 2, size: '1536x1024', quality: 'high', user: 'someone' });
    assert.equal(r.status, 200);
    assert.equal(r.body.data.length, 2);
    assert.ok(r.body.data[0].b64_json);
    // Only the mapped model, the prompt, the count and the size are forwarded.
    assert.deepEqual(sent().map((x) => [x.path, x.body]), [['/v1/images/generations', { model: 'acme-llm-image', prompt: 'a red car', n: 2, size: '1536x1024' }]]);
    const rows = h.cp.db.prepare("SELECT endpoint, role, upstream_model, bucket, images, total_tokens, content FROM llm_usage WHERE user_name = 'image1'").all() as any[];
    assert.deepEqual(rows.map((x) => ({ ...x })), [{ endpoint: 'images', role: 'image', upstream_model: 'acme-llm-image', bucket: 'image', images: 2, total_tokens: 0, content: null }]);
  });

  it('refuses the image role on other endpoints and other roles on the image endpoint', async () => {
    const onChat = await h.call('POST', '/llm/v1/chat/completions', token, { model: 'image', messages: [{ role: 'user', content: 'hi' }] });
    assert.equal(onChat.status, 400);
    assert.equal(onChat.body.error.code, 'INVALID_REQUEST');
    assert.equal((await h.call('POST', '/llm/v1/embeddings', token, { model: 'image', input: ['x'] })).status, 400);
    const mainOnImages = await image(token, { model: 'main' });
    assert.equal(mainOnImages.status, 400);
    assert.equal(mainOnImages.body.error.code, 'INVALID_REQUEST');
    for (const bad of [{ n: 5 }, { n: 0 }, { n: 1.5 }, { size: '512x512' }, { prompt: '' }, { prompt: 'x'.repeat(4001) }]) {
      const r = await image(token, bad);
      assert.equal(r.status, 400, JSON.stringify(bad));
      assert.equal(r.body.error.code, 'INVALID_REQUEST');
    }
    assert.equal(sent().length, 0);
  });

  it('rejects an image request that does not fit the remaining image budget, before forwarding', async () => {
    const u = await h.token('imgbudget1');
    h.cp.db.prepare(`INSERT INTO llm_usage (at, subject, user_name, endpoint, role, bucket, images, total_tokens, outcome, duration_ms)
      VALUES (?, 'sub-imgbudget1', 'imgbudget1', 'images', 'image', 'image', ?, 0, 'ok', 1)`).run(new Date().toISOString(), h.cfg.gateway.imagesDaily - 1);
    const two = await image(u, { n: 2 });
    assert.equal(two.status, 429);
    assert.equal(two.body.error.code, 'QUOTA_EXCEEDED');
    assert.ok(Date.parse(two.body.error.reset_at) > Date.now());
    assert.equal(sent().length, 0);
    assert.equal((await image(u)).status, 200);
    assert.equal((await image(u)).body.error.code, 'QUOTA_EXCEEDED');
    // The chat budget is unaffected.
    assert.equal((await chat(u)).status, 200);
    const chatUsed = h.cp.db.prepare("SELECT COALESCE(SUM(total_tokens),0) AS t FROM llm_usage WHERE user_name = 'imgbudget1' AND bucket = 'chat'").get() as any;
    assert.equal(chatUsed.t, 12);
  });

  it('gives an image request the image timeout and answers TIMEOUT when no backend finishes in time', async () => {
    const saved = h.cfg.gateway.imageTimeoutMs;
    h.cfg.gateway.imageTimeoutMs = 300;
    const hang: FakeUpstream['handler'] = () => undefined;
    a.handler = hang;
    b.handler = hang;
    try {
      const r = await image(token);
      assert.equal(r.status, 504);
      assert.equal(r.body.error.code, 'TIMEOUT');
      assert.equal(sent().length, 2);  // tried on both backends
    } finally {
      h.cfg.gateway.imageTimeoutMs = saved;
    }
  });

  it('lets requests in flight overshoot a soft budget, then rejects the next one', async () => {
    const u = await h.token('overshoot1');
    h.cp.db.prepare(`INSERT INTO llm_usage (at, subject, user_name, endpoint, role, bucket, total_tokens, outcome, duration_ms)
      VALUES (?, 'sub-overshoot1', 'overshoot1', 'chat', 'main', 'chat', ?, 'ok', 1)`).run(new Date().toISOString(), h.cfg.gateway.budgetTokens - 5);
    a.handler = delayed(defaultHandler.a!, 200);
    b.handler = delayed(defaultHandler.b!, 200);
    const both = await Promise.all([chat(u), chat(u)]);
    assert.deepEqual(both.map((r) => r.status), [200, 200]);
    const used = (h.cp.db.prepare("SELECT SUM(total_tokens) AS t FROM llm_usage WHERE user_name = 'overshoot1' AND bucket = 'chat'").get() as any).t;
    assert.ok(used > h.cfg.gateway.budgetTokens);
    const next = await chat(u);
    assert.equal(next.status, 429);
    assert.equal(next.body.error.code, 'QUOTA_EXCEEDED');
  });
});

const delayed = (base: FakeUpstream['handler'], ms: number): FakeUpstream['handler'] => (p, body, res) => {
  setTimeout(() => base(p, body, res), ms);
};

describe('gateway at telemetry level content', () => {
  let up: FakeUpstream;
  let h: Harness;
  before(async () => {
    up = await fakeUpstream();
    h = await startHarness((c) => {
      c.gateway.upstreams = [{ name: 'a', baseUrl: up.url, apiKey: KEY_A, maxConcurrent: 0 }];
      c.gateway.telemetryLevel = 'content';
    });
  });
  after(async () => {
    await h.close();
    await up.close();
  });

  it('keeps only the prompt of an image request, never the images', async () => {
    const t = await h.token('content2');
    assert.equal((await h.call('POST', '/llm/v1/images/generations', t, { model: 'image', prompt: 'a blue bicycle' })).status, 200);
    const row = h.cp.db.prepare("SELECT content, images FROM llm_usage WHERE user_name = 'content2'").get() as any;
    assert.deepEqual(JSON.parse(row.content), { request: { prompt: 'a blue bicycle', n: 1 } });
    assert.equal(row.images, 1);
  });

  it('stores chat content, streamed or not, and never embedding or rerank inputs', async () => {
    const t = await h.token('content1');
    await h.call('POST', '/llm/v1/chat/completions', t, { model: 'main', messages: [{ role: 'user', content: 'plain question' }] });
    await h.call('POST', '/llm/v1/chat/completions', t, { model: 'main', stream: true, messages: [{ role: 'user', content: 'streamed question' }] });
    await h.call('POST', '/llm/v1/embeddings', t, { model: 'embed', input: ['private chunk text'] });
    await h.call('POST', '/llm/v1/rerank', t, { model: 'rerank', query: 'private query', documents: ['private passage'] });
    await waitUntil(() => (h.cp.db.prepare("SELECT COUNT(*) AS n FROM llm_usage WHERE user_name = 'content1'").get() as any).n === 4);
    const rows = h.cp.db.prepare("SELECT endpoint, content FROM llm_usage WHERE user_name = 'content1' ORDER BY id").all() as any[];
    const plain = JSON.parse(rows[0].content);
    assert.equal(plain.request.messages[0].content, 'plain question');
    assert.equal(plain.response.choices[0].message.content, 'Hello');
    const streamed = JSON.parse(rows[1].content);
    assert.equal(streamed.request.messages[0].content, 'streamed question');
    assert.equal(streamed.response.content, 'Hello');
    assert.deepEqual(rows.slice(2).map((r) => [r.endpoint, r.content]), [['embeddings', null], ['rerank', null]]);
    const all = JSON.stringify(h.cp.db.prepare('SELECT * FROM llm_usage').all());
    for (const secret of ['private chunk text', 'private query', 'private passage']) assert.ok(!all.includes(secret), `stored ${secret}`);
  });
});

describe('gateway budgets across a restart', () => {
  it('keeps the used budget in SQLite, so a restarted gateway still rejects', async () => {
    const dir = mkdtempSync(join(tmpdir(), 'skowork-gw-'));
    const dbPath = join(dir, 'cp.db');
    const up = await fakeUpstream();
    const setup = (c: Config) => {
      c.dbPath = dbPath;
      c.gateway.upstreams = [{ name: 'a', baseUrl: up.url, apiKey: KEY_A, maxConcurrent: 0 }];
      c.gateway.budgetTokens = 20; // two answers of 12 tokens use it up
    };
    try {
      let h = await startHarness(setup);
      const ask = (hh: Harness, t: string) => hh.call('POST', '/llm/v1/chat/completions', t, { model: 'main', messages: [{ role: 'user', content: 'hi' }] });
      let t = await h.token('restart1');
      assert.equal((await ask(h, t)).status, 200);
      assert.equal((await ask(h, t)).status, 200);
      assert.equal((await ask(h, t)).body.error.code, 'QUOTA_EXCEEDED');
      await h.close();

      h = await startHarness(setup);
      t = await h.token('restart1');
      const r = await ask(h, t);
      assert.equal(r.status, 429);
      assert.equal(r.body.error.code, 'QUOTA_EXCEEDED');
      assert.ok(Date.parse(r.body.error.reset_at) > Date.now());
      assert.equal((await ask(h, await h.token('restart2'))).status, 200); // another user is unaffected
      await h.close();
    } finally {
      await up.close();
      rmSync(dir, { recursive: true, force: true });
    }
  });
});

describe('gateway failover soak', () => {
  it('loses no request when one of two backends is killed under load', async () => {
    const a = await fakeUpstream();
    const b = await fakeUpstream();
    // Answers arrive whole after a delay, so a kill hits requests before their first byte (a stream cut after its
    // first byte is never retried: Backend Failover).
    a.handler = delayed(a.handler, 25);
    b.handler = delayed(b.handler, 25);
    const h = await startHarness((c) => {
      c.gateway.upstreams = [
        { name: 'a', baseUrl: a.url, apiKey: KEY_A, maxConcurrent: 0 },
        { name: 'b', baseUrl: b.url, apiKey: KEY_B, maxConcurrent: 0 },
      ];
      c.gateway.requestsPerMinute = 100_000;
      c.gateway.concurrentRequests = 100;
    });
    try {
      const users = await Promise.all(Array.from({ length: 8 }, (_, i) => h.token(`soak${i}`)));
      const total = 400;
      let started = 0;
      let killed = false;
      const failures: string[] = [];
      const worker = async (w: number) => {
        while (started < total) {
          const n = started++;
          if (n === Math.floor(total / 3)) {
            killed = true;
            void a.close(); // kill backend a: in-flight connections are destroyed, new ones refused
          }
          const stream = n % 2 === 0;
          const r = await h.call('POST', '/llm/v1/chat/completions', users[w % users.length]!, {
            model: 'main', stream, messages: [{ role: 'user', content: `request ${n}` }],
          });
          const ok = r.status === 200 && (stream ? /\[DONE\]/.test(String(r.body)) : r.body?.choices?.[0]?.message?.content === 'Hello');
          if (!ok) failures.push(`${n}: ${r.status} ${JSON.stringify(r.body).slice(0, 120)}`);
        }
      };
      await Promise.all(Array.from({ length: 16 }, (_, w) => worker(w)));
      assert.ok(killed);
      assert.deepEqual(failures, []);
      assert.equal(h.cp.gateway.pool.backends[0]!.healthy, false);
      const served = b.requests.filter((r) => r.path.endsWith('/chat/completions')).length;
      assert.ok(served >= (total * 2) / 3, `b served ${served}`);
      const outcomes = h.cp.db.prepare("SELECT outcome, COUNT(*) AS n FROM llm_usage WHERE endpoint = 'chat' GROUP BY outcome").all() as any[];
      assert.deepEqual(outcomes.map((o) => [o.outcome, o.n]), [['ok', total]]);
    } finally {
      await h.close();
      await b.close();
      await a.close(); // a no-op when the kill already closed it
    }
  });
});
