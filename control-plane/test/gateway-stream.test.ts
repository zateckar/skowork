// control-plane/llm-gateway streaming (fix-long-thread-model-calls D1, D2, D7): keep-alives before the first token, error
// events after the headers, the three stream limits and the prompt cache audit. The timeouts are small settings, not
// long sleeps; the in-test upstreams delay, fail or stall on demand.
import { after, afterEach, before, describe, it } from 'node:test';
import assert from 'node:assert/strict';
import type { ServerResponse } from 'node:http';
import { fakeUpstream, json, startHarness, waitUntil, type FakeUpstream, type Harness } from './helpers.ts';

const KEY_A = 'upstream-key-a-0123456789';
const KEY_B = 'upstream-key-b-9876543210';

type Handler = FakeUpstream['handler'];
const delayed = (base: Handler, ms: number): Handler => (p, body, res) => {
  setTimeout(() => base(p, body, res), ms);
};
const frame = (res: ServerResponse, data: unknown) => res.write(`data: ${JSON.stringify(data)}\n\n`);
/** The data events of an SSE body, without comments and without the [DONE] marker. */
const events = (text: string) => text.split('\n\n').filter((f) => f.startsWith('data: ') && f !== 'data: [DONE]').map((f) => JSON.parse(f.slice(6)));

describe('gateway streams', () => {
  let a: FakeUpstream;
  let b: FakeUpstream;
  let h: Harness;
  let defaultA: Handler;
  let defaultB: Handler;

  before(async () => {
    a = await fakeUpstream();
    b = await fakeUpstream();
    defaultA = a.handler;
    defaultB = b.handler;
    h = await startHarness((c) => {
      c.gateway.upstreams = [
        { name: 'a', baseUrl: a.url, apiKey: KEY_A, maxConcurrent: 0 },
        { name: 'b', baseUrl: b.url, apiKey: KEY_B, maxConcurrent: 0 },
      ];
      c.gateway.healthCheckIntervalMs = 100;
      c.gateway.keepAliveMs = 100;
      c.gateway.firstTokenTimeoutMs = 1500;
      c.gateway.streamIdleTimeoutMs = 1000;
      c.gateway.streamMaxMs = 10_000;
      c.gateway.requestTimeoutMs = 150; // must not cut a stream body
    });
  });
  afterEach(() => {
    a.handler = defaultA;
    b.handler = defaultB;
    a.requests.length = 0;
    b.requests.length = 0;
    for (const be of h.cp.gateway.pool.backends) be.healthy = true;
  });
  after(async () => {
    await h.close();
    await a.close();
    await b.close();
  });

  /** A streamed chat request with raw fetch, so the time to the response headers can be measured. */
  async function stream(user: string, body: Record<string, unknown> = {}) {
    const t = await h.token(user);
    const t0 = Date.now();
    const r = await fetch(h.base + '/llm/v1/chat/completions', {
      method: 'POST',
      headers: { authorization: `Bearer ${t}`, 'content-type': 'application/json' },
      body: JSON.stringify({ model: 'main', stream: true, messages: [{ role: 'user', content: 'hi' }], ...body }),
    });
    const headersMs = Date.now() - t0;
    const text = await r.text();
    return { status: r.status, type: r.headers.get('content-type') ?? '', text, headersMs };
  }
  const audit = (user: string) =>
    h.cp.db.prepare("SELECT outcome, ttft_ms, cached_tokens FROM llm_usage WHERE user_name = ? AND endpoint = 'chat' ORDER BY id DESC LIMIT 1").get(user) as any;
  const chatCalls = (reqs: FakeUpstream['requests']) => reqs.filter((r) => r.path.endsWith('/chat/completions')).length;

  it('sends the headers once a backend is held, and keep-alive comments while the first token is delayed', async () => {
    a.handler = delayed(defaultA, 500);
    b.handler = delayed(defaultB, 500);
    const r = await stream('ka1');
    assert.equal(r.status, 200);
    assert.match(r.type, /text\/event-stream/);
    assert.ok(r.headersMs < 400, `headers took ${r.headersMs} ms`);
    const first = r.text.indexOf('Hello');
    const beats = r.text.split('\n').filter((l) => l === ': keep-alive').length;
    assert.ok(beats >= 2, `${beats} keep-alive comments`);
    assert.ok(r.text.indexOf(': keep-alive') < first);
    assert.match(r.text, /Hello[\s\S]*\[DONE\]/);
    await waitUntil(() => audit('ka1')?.outcome === 'ok');
  });

  it('fails over when a backend dies before its first byte, and the client sees one normal stream', async () => {
    a.handler = (_p, _b, res) => setTimeout(() => res.socket?.destroy(), 250);
    const r = await stream('fo1');
    assert.equal(r.status, 200);
    assert.match(r.text, /: keep-alive/);
    assert.equal(r.text.match(/Hello/g)?.length, 1);
    assert.equal(r.text.match(/\[DONE\]/g)?.length, 1);
    assert.equal(events(r.text).some((e) => 'error' in e), false);
    assert.equal(chatCalls(a.requests), 1);
    assert.equal(chatCalls(b.requests), 1);
    await waitUntil(() => audit('fo1')?.outcome === 'ok');
  });

  it('ends with one error event and no [DONE] when every backend fails after the headers', async () => {
    const dead: Handler = (_p, _b, res) => setTimeout(() => res.socket?.destroy(), 250);
    a.handler = dead;
    b.handler = dead;
    const r = await stream('dead1');
    assert.equal(r.status, 200);
    assert.match(r.type, /text\/event-stream/);
    assert.doesNotMatch(r.text, /\[DONE\]/);
    const errors = events(r.text).filter((e) => 'error' in e);
    assert.equal(errors.length, 1);
    assert.deepEqual(errors[0].error, { code: 'MODEL_UNAVAILABLE', message: 'The model server is not available.', reason: 'backend unavailable' });
    assert.ok(!r.text.includes(KEY_A) && !r.text.includes(KEY_B) && !r.text.includes(a.url) && !r.text.includes('127.0.0.1'));
    await waitUntil(() => audit('dead1')?.outcome === 'rejected:model_unavailable');
  });

  it('refuses a streamed request before any backend is held with an HTTP error and no stream headers', async () => {
    const t = await h.token('refused1');
    h.cp.db.prepare(`INSERT INTO llm_usage (at, subject, user_name, endpoint, role, bucket, total_tokens, outcome, duration_ms)
      VALUES (?, 'sub-refused1', 'refused1', 'chat', 'main', 'chat', 2000000, 'ok', 1)`).run(new Date().toISOString());
    const r = await fetch(h.base + '/llm/v1/chat/completions', {
      method: 'POST',
      headers: { authorization: `Bearer ${t}`, 'content-type': 'application/json' },
      body: JSON.stringify({ model: 'main', stream: true, messages: [{ role: 'user', content: 'hi' }] }),
    });
    assert.equal(r.status, 429);
    assert.match(r.headers.get('content-type') ?? '', /application\/json/);
    assert.equal((await r.json()).error.code, 'QUOTA_EXCEEDED');
    assert.equal(chatCalls(a.requests) + chatCalls(b.requests), 0);
  });

  it('completes a long stream whose gaps are longer than the request timeout but shorter than the idle limit', async () => {
    const slow: Handler = (_p, _b, res) => {
      res.writeHead(200, { 'content-type': 'text/event-stream' });
      const parts = ['a', 'b', 'c', 'd'];
      let i = 0;
      const tick = () => {
        if (res.destroyed) return;
        if (i < parts.length) {
          frame(res, { choices: [{ delta: { content: parts[i] } }] });
          i++;
          setTimeout(tick, 250);
          return;
        }
        frame(res, { choices: [{ delta: {}, finish_reason: 'stop' }] });
        frame(res, { choices: [], usage: { prompt_tokens: 10, completion_tokens: 4, total_tokens: 14 } });
        res.end('data: [DONE]\n\n');
      };
      tick();
    };
    a.handler = slow;
    b.handler = slow;
    const t0 = Date.now();
    const r = await stream('long1');
    assert.ok(Date.now() - t0 > h.cfg.gateway.requestTimeoutMs * 3);
    assert.equal(r.status, 200);
    assert.match(r.text, /\[DONE\]/);
    assert.equal(events(r.text).filter((e) => 'error' in e).length, 0);
    await waitUntil(() => audit('long1')?.outcome === 'ok');
  });

  it('answers TIMEOUT in one error event when no backend sends a byte within the first-token limit', async () => {
    const saved = h.cfg.gateway.firstTokenTimeoutMs;
    h.cfg.gateway.firstTokenTimeoutMs = 300;
    const silent: Handler = () => undefined;
    a.handler = silent;
    b.handler = silent;
    try {
      const r = await stream('timeout1');
      assert.equal(r.status, 200);
      assert.match(r.text, /: keep-alive/);
      assert.doesNotMatch(r.text, /\[DONE\]/);
      const errors = events(r.text).filter((e) => 'error' in e);
      assert.equal(errors.length, 1);
      assert.equal(errors[0].error.code, 'TIMEOUT');
      await waitUntil(() => audit('timeout1')?.outcome === 'rejected:timeout');
    } finally {
      h.cfg.gateway.firstTokenTimeoutMs = saved;
    }
  });

  it('cuts a stream with an idle gap after its first byte: no [DONE], outcome truncated, no error event', async () => {
    const saved = h.cfg.gateway.streamIdleTimeoutMs;
    h.cfg.gateway.streamIdleTimeoutMs = 300;
    const stall: Handler = (_p, _b, res) => {
      res.writeHead(200, { 'content-type': 'text/event-stream' });
      frame(res, { choices: [{ delta: { content: 'par' } }] });
    };
    a.handler = stall;
    b.handler = stall;
    try {
      const r = await stream('idle1');
      assert.equal(r.status, 200);
      assert.match(r.text, /par/);
      assert.doesNotMatch(r.text, /\[DONE\]/);
      assert.equal(events(r.text).filter((e) => 'error' in e).length, 0);
      await waitUntil(() => audit('idle1')?.outcome === 'truncated');
    } finally {
      h.cfg.gateway.streamIdleTimeoutMs = saved;
    }
  });

  it('cuts a stream that runs past the maximum stream duration', async () => {
    const saved = h.cfg.gateway.streamMaxMs;
    h.cfg.gateway.streamMaxMs = 400;
    const endless: Handler = (_p, _b, res) => {
      res.writeHead(200, { 'content-type': 'text/event-stream' });
      const tick = () => {
        if (res.destroyed) return;
        frame(res, { choices: [{ delta: { content: 'x' } }] });
        setTimeout(tick, 100);
      };
      tick();
    };
    a.handler = endless;
    b.handler = endless;
    try {
      const r = await stream('max1');
      assert.doesNotMatch(r.text, /\[DONE\]/);
      assert.ok(r.text.includes('x'));
      await waitUntil(() => audit('max1')?.outcome === 'truncated');
    } finally {
      h.cfg.gateway.streamMaxMs = saved;
    }
  });

  it('records the time to the first byte and the cached prompt tokens of a streamed answer', async () => {
    const cached: Handler = delayed((_p, _b, res) => {
      res.writeHead(200, { 'content-type': 'text/event-stream' });
      frame(res, { choices: [{ delta: { content: 'Hi' } }] });
      frame(res, { choices: [{ delta: {}, finish_reason: 'stop' }] });
      frame(res, { choices: [], usage: { prompt_tokens: 43000, completion_tokens: 10, total_tokens: 43010, prompt_tokens_details: { cached_tokens: 41000 } } });
      res.end('data: [DONE]\n\n');
    }, 120);
    a.handler = cached;
    b.handler = cached;
    await stream('cache1');
    await waitUntil(() => audit('cache1')?.outcome === 'ok');
    const row = audit('cache1');
    assert.equal(row.cached_tokens, 41000);
    assert.ok(row.ttft_ms >= 100, `ttft ${row.ttft_ms}`);
  });

  it('records the cached prompt tokens of a non-streamed answer', async () => {
    a.handler = (_p, _b, res) => json(res, 200, {
      choices: [{ message: { role: 'assistant', content: 'Hi' }, finish_reason: 'stop' }],
      usage: { prompt_tokens: 43000, completion_tokens: 10, total_tokens: 43010, prompt_tokens_details: { cached_tokens: 42000 } },
    });
    b.handler = a.handler;
    const t = await h.token('cache2');
    assert.equal((await h.call('POST', '/llm/v1/chat/completions', t, { model: 'main', messages: [{ role: 'user', content: 'hi' }] })).status, 200);
    await waitUntil(() => audit('cache2')?.outcome === 'ok');
    assert.equal(audit('cache2').cached_tokens, 42000);
    assert.ok(audit('cache2').ttft_ms >= 0);
  });

  it('leaves the cached prompt tokens empty when the upstream does not report them', async () => {
    const t = await h.token('cache3');
    assert.equal((await stream('cache3')).status, 200);
    assert.equal((await h.call('POST', '/llm/v1/chat/completions', t, { model: 'main', messages: [{ role: 'user', content: 'hi' }] })).status, 200);
    await waitUntil(() => audit('cache3')?.outcome === 'ok');
    const row = audit('cache3');
    assert.equal(row.cached_tokens, null);
    assert.equal(typeof row.ttft_ms, 'number');
  });
});
