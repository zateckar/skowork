// Model settings, reasoning effort and fallback (add-model-fallback-and-effort tasks 2.1-2.4): the admin API's effort probe
// and validation, the effort on requests, the fallback with its fit check and cooldown, and the usage audit. The upstream
// is one in-test server whose primary model stops answering on demand. The timeouts are small settings; a fallback wait
// of 1 s is written to the database, since the admin API accepts 5 s and up (its own test checks that).
import { after, afterEach, before, describe, it } from 'node:test';
import assert from 'node:assert/strict';
import type { ServerResponse } from 'node:http';
import { fakeUpstream, json, startHarness, waitUntil, type FakeUpstream, type Harness } from './helpers.ts';

const KEY_A = 'upstream-key-a-0123456789';
/** The efforts the in-test upstream accepts for a probe; any other value is refused as the real model does. */
const ACCEPTED = ['low', 'medium', 'xhigh'];

type Handler = FakeUpstream['handler'];
const isChat = (p: string) => p.endsWith('/chat/completions');
/** The one-token probe of an effort (D1) is recognisable by max_tokens 1; no other request in these tests sends it. */
const isProbe = (body: any) => body?.max_tokens === 1;
const frame = (res: ServerResponse, data: unknown) => res.write(`data: ${JSON.stringify(data)}\n\n`);
const events = (text: string) => text.split('\n\n').filter((f) => f.startsWith('data: ') && f !== 'data: [DONE]').map((f) => JSON.parse(f.slice(6)));
const longText = 'x'.repeat(300_000); // about 75,000 tokens: over the fallback's 65,536, under the primary's 131,072

describe('model settings, fallback and effort', () => {
  let a: FakeUpstream;
  let h: Harness;
  let script: Handler;
  let normal: Handler;
  let adminTok: string;
  let connId: number;

  before(async () => {
    a = await fakeUpstream();
    const plain = a.handler;
    normal = (p, body, res) => {
      if (p.endsWith('/models')) return json(res, 200, { data: [{ id: 'acme-llm-primary' }, { id: 'acme-llm-secondary' }] });
      if (isChat(p) && isProbe(body) && body.reasoning_effort !== undefined && !ACCEPTED.includes(body.reasoning_effort)) {
        return json(res, 400, { error: { message: `Unsupported reasoning_effort "${body.reasoning_effort}". Supported values: low, medium, xhigh. Key ${KEY_A} at https://llm.example.test/v1` } });
      }
      plain(p, body, res);
    };
    script = normal;
    a.handler = (p, body, res) => script(p, body, res);
    h = await startHarness((c) => {
      c.gateway.upstreams = [{ name: 'primary', baseUrl: a.url, apiKey: KEY_A, maxConcurrent: 0 }];
      c.gateway.healthCheckIntervalMs = 100;
      c.gateway.keepAliveMs = 100;
      c.gateway.firstTokenTimeoutMs = 1500;
      c.gateway.streamIdleTimeoutMs = 1000;
      c.gateway.streamMaxMs = 10_000;
      c.gateway.requestTimeoutMs = 1500;
      c.gateway.fallbackCooldownMs = 0;
    });
    adminTok = await h.token('admin1');
    connId = (await h.call('GET', '/api/v1/admin/llm/connections', adminTok)).body.connections[0].id;
    // Lists the two models, so that they are not marked missing (routing ignores that flag; the fallback choice does not).
    await h.call('POST', `/api/v1/admin/llm/connections/${connId}/test`, adminTok);
    // A third chat model, only for the "is the fallback of another model" check.
    h.cp.db.prepare("INSERT INTO llm_models (connection_id, model_id, kind, context_window, tools) VALUES (?, 'acme-llm-third', 'chat', 32768, 1)").run(connId);
  });
  afterEach(() => {
    script = normal;
    a.requests.length = 0;
    healthy();
    h.cp.db.exec(
      "UPDATE llm_models SET reasoning_effort = NULL, fallback_model = NULL, fallback_after_s = NULL, tools = 1, " +
        "images = CASE WHEN model_id = 'acme-llm-primary' THEN 1 ELSE 0 END",
    );
    h.cfg.gateway.fallbackCooldownMs = 0;
  });
  after(async () => {
    await h.close();
    await a.close();
  });

  const healthy = () => {
    for (const be of h.cp.gateway.pool.backends) be.healthy = true;
  };
  const ad = (method: string, path: string, body?: unknown) => h.call(method, `/api/v1/admin/llm${path}`, adminTok, body);
  const put = (id: string, body: object) => ad('PUT', `/connections/${connId}/models/${id}`, body);
  const modelRow = async (id: string) => (await ad('GET', '/connections')).body.connections[0].models.find((m: any) => m.id === id);
  const chat = async (user: string, body: Record<string, unknown> = {}) =>
    h.call('POST', '/llm/v1/chat/completions', await h.token(user), { model: 'main', messages: [{ role: 'user', content: 'hi' }], ...body });
  /** A streamed chat request with raw fetch, so the text of the stream is kept as sent. */
  async function streamed(user: string) {
    const r = await fetch(h.base + '/llm/v1/chat/completions', {
      method: 'POST',
      headers: { authorization: `Bearer ${await h.token(user)}`, 'content-type': 'application/json' },
      body: JSON.stringify({ model: 'main', stream: true, messages: [{ role: 'user', content: 'hi' }] }),
    });
    return { status: r.status, text: await r.text() };
  }
  const audit = (user: string) =>
    h.cp.db.prepare("SELECT outcome, answered_by, fallback, upstream_model FROM llm_usage WHERE user_name = ? AND endpoint = 'chat' ORDER BY id DESC LIMIT 1").get(user) as any;
  const auditOf = async (user: string) => {
    await waitUntil(() => Boolean(audit(user)));
    return audit(user);
  };
  /** Chat requests of the upstream that were answered, not probes, for one model id. */
  const served = (model: string) => a.requests.filter((r) => isChat(r.path) && !isProbe(r.body) && r.body.model === model);
  /** The primary model answers as `behaviour` says; the other requests get the normal answer. */
  const primaryDoes =
    (behaviour: Handler): Handler =>
    (p, body, res) =>
      isChat(p) && !isProbe(body) && body.model === 'acme-llm-primary' ? behaviour(p, body, res) : normal(p, body, res);
  const silent: Handler = () => undefined;
  const status =
    (code: number, message: string): Handler =>
    (_p, _b, res) =>
      json(res, code, { error: { message } });
  const setFallback = (fallback: string | null, after: number | null = 1) =>
    h.cp.db.prepare('UPDATE llm_models SET fallback_model = ?, fallback_after_s = ? WHERE model_id = ?').run(fallback, after, 'acme-llm-primary');

  // ---- 2.1 model settings: the effort check, validation and the Test ------------------------------------------------

  it('stores an effort the model accepts after a one-token probe, and refuses another with the upstream reason', async () => {
    const ok = await put('acme-llm-primary', { reasoning_effort: 'low' });
    assert.equal(ok.status, 200);
    assert.equal((await modelRow('acme-llm-primary')).reasoning_effort, 'low');
    const probe = a.requests.filter((r) => isChat(r.path) && isProbe(r.body)).at(-1)!;
    assert.deepEqual([probe.body.model, probe.body.stream, probe.body.max_tokens, probe.body.reasoning_effort], ['acme-llm-primary', false, 1, 'low']);
    assert.equal(probe.headers.authorization, `Bearer ${KEY_A}`);

    const refused = await put('acme-llm-primary', { reasoning_effort: 'high' });
    assert.equal(refused.status, 400);
    assert.equal(refused.body.error.code, 'INVALID_REQUEST');
    assert.equal(refused.body.error.field, 'reasoning_effort');
    assert.match(refused.body.error.message, /"high"/);
    assert.match(refused.body.error.message, /Supported values: low, medium, xhigh/);
    // The upstream's text is kept, but its credential and address are not.
    assert.ok(!refused.body.error.message.includes(KEY_A));
    assert.ok(!refused.body.error.message.includes('llm.example.test'));
    assert.equal((await modelRow('acme-llm-primary')).reasoning_effort, 'low', 'the stored value is kept');
    const refusals = h.cp.db.prepare("SELECT outcome, details FROM audit WHERE action = 'llm.model.effort_refused'").all() as any[];
    assert.equal(refusals.length, 1);
    assert.equal(refusals[0].outcome, 'refused');
    assert.ok(!JSON.stringify(refusals).includes(KEY_A));
    await put('acme-llm-primary', { reasoning_effort: null });
  });

  it('refuses an effort that cannot be checked, and clears one without a probe', async () => {
    script = (p, body, res) => (isChat(p) && isProbe(body) ? res.socket?.destroy() : normal(p, body, res));
    const before = a.requests.length;
    const r = await put('acme-llm-primary', { reasoning_effort: 'low' });
    assert.equal(r.status, 400);
    assert.match(r.body.error.message, /could not be checked/);
    assert.equal(r.body.error.reason, 'unchecked');
    assert.equal((await modelRow('acme-llm-primary')).reasoning_effort, null);
    const cleared = await put('acme-llm-primary', { reasoning_effort: null });
    assert.equal(cleared.status, 200);
    assert.equal(a.requests.slice(before).filter((x) => isProbe(x.body)).length, 1, 'only the first save probed');
  });

  it('validates a fallback: a known chat model, not itself, with no fallback of its own, and not one that has a fallback', async () => {
    const self = await put('acme-llm-primary', { fallback_model: 'acme-llm-primary' });
    assert.equal(self.status, 400);
    assert.match(self.body.error.message, /own fallback/);
    const unknown = await put('acme-llm-primary', { fallback_model: 'no-such-model' });
    assert.equal(unknown.status, 400);
    assert.equal(unknown.body.error.field, 'fallback_model');
    const short = await put('acme-llm-primary', { fallback_model: 'acme-llm-secondary', fallback_after_s: 3 });
    assert.equal(short.status, 400);
    assert.equal(short.body.error.field, 'fallback_after_s');
    assert.equal((await modelRow('acme-llm-primary')).fallback_model, null, 'nothing was stored');

    // The secondary takes the primary as its fallback: the primary cannot then name the secondary.
    assert.equal((await put('acme-llm-secondary', { fallback_model: 'acme-llm-primary' })).status, 200);
    const chain = await put('acme-llm-primary', { fallback_model: 'acme-llm-secondary' });
    assert.equal(chain.status, 400);
    assert.match(chain.body.error.message, /fallback of its own/);
    assert.equal((await put('acme-llm-secondary', { fallback_model: null })).status, 200);

    // The primary falls back to the secondary, which then lists the primary as a fallback of its own.
    const set = await put('acme-llm-primary', { fallback_model: 'acme-llm-secondary', fallback_after_s: 7 });
    assert.equal(set.status, 200);
    const primary = await modelRow('acme-llm-primary');
    assert.deepEqual([primary.fallback_model, primary.fallback_after_s], ['acme-llm-secondary', 7]);
    assert.deepEqual((await modelRow('acme-llm-secondary')).fallback_of, ['primary/acme-llm-primary']);
    // A model that is a fallback cannot have one: the third model is refused for the secondary.
    const isFallback = await put('acme-llm-secondary', { fallback_model: 'acme-llm-third' });
    assert.equal(isFallback.status, 400);
    assert.match(isFallback.body.error.message, /is the fallback of primary\/acme-llm-primary/);
    assert.equal((await modelRow('acme-llm-secondary')).fallback_model, null);
    assert.equal((await put('acme-llm-primary', { fallback_model: null })).status, 200);
  });

  it('Connection Test reports a stored effort that the model now refuses, and keeps its other fields', async () => {
    assert.equal((await put('acme-llm-primary', { reasoning_effort: 'low' })).status, 200);
    script = (p, body, res) => (isChat(p) && isProbe(body) && body.reasoning_effort === 'low' ? json(res, 400, { error: { message: 'low is no longer supported' } }) : normal(p, body, res));
    const stale = await ad('POST', `/connections/${connId}/test`);
    assert.equal(stale.status, 200);
    assert.equal(stale.body.status, 'ok');
    assert.deepEqual(stale.body.effort_failures.map((f: any) => [f.model, f.reasoning_effort]), [['acme-llm-primary', 'low']]);
    assert.match(stale.body.effort_failures[0].reason, /no longer supported/);
    script = normal;
    assert.deepEqual((await ad('POST', `/connections/${connId}/test`)).body.effort_failures, []);
    await put('acme-llm-primary', { reasoning_effort: null });
  });

  // ---- 2.2 the effort on requests -------------------------------------------------------------------------------------

  it("sends the stored effort in place of the client's, and leaves the body alone when none is stored", async () => {
    await put('acme-llm-primary', { reasoning_effort: 'low' });
    assert.equal((await chat('eff1', { reasoning_effort: 'high' })).status, 200);
    assert.equal(served('acme-llm-primary').at(-1)!.body.reasoning_effort, 'low');

    await put('acme-llm-primary', { reasoning_effort: null });
    a.requests.length = 0;
    assert.equal((await chat('eff2', { reasoning_effort: 'high' })).status, 200);
    assert.equal(served('acme-llm-primary').at(-1)!.body.reasoning_effort, 'high');
    assert.equal((await chat('eff3')).status, 200);
    assert.ok(!('reasoning_effort' in served('acme-llm-primary').at(-1)!.body));
    assert.equal((await auditOf('eff3')).answered_by, 'acme-llm-primary');
    assert.equal((await auditOf('eff3')).fallback, null);
  });

  // ---- 2.3 fallback, cooldown and 2.4 audit ---------------------------------------------------------------------------

  it('a slow first byte falls back a streamed request: keep-alives come first, the answer is the fallback\'s, the audit names it', async () => {
    setFallback('acme-llm-secondary', 1);
    script = primaryDoes(silent);
    const r = await streamed('fb1');
    assert.equal(r.status, 200);
    assert.ok(r.text.indexOf(': keep-alive') >= 0 && r.text.indexOf(': keep-alive') < r.text.indexOf('Hello'));
    assert.equal(r.text.match(/Hello/g)?.length, 1);
    assert.equal(r.text.match(/\[DONE\]/g)?.length, 1);
    assert.equal(events(r.text).some((e) => 'error' in e), false);
    assert.equal(served('acme-llm-primary').length, 1);
    assert.equal(served('acme-llm-secondary').length, 1);
    const row = await auditOf('fb1');
    assert.deepEqual([row.outcome, row.answered_by, row.fallback, row.upstream_model], ['ok', 'acme-llm-secondary', 'timeout', 'acme-llm-primary']);
  });

  it('a 503 falls back a request, and so does a 429 on a stream: the reasons are unavailable and capacity', async () => {
    setFallback('acme-llm-secondary', 1);
    script = primaryDoes(status(503, 'overloaded'));
    const unavailable = await chat('fb2');
    assert.equal(unavailable.status, 200);
    assert.equal(unavailable.body.choices[0].message.content, 'Hello');
    assert.equal((await auditOf('fb2')).fallback, 'unavailable');

    script = primaryDoes(status(429, 'busy'));
    healthy();
    const capacity = await streamed('fb3');
    assert.match(capacity.text, /Hello/);
    assert.equal((await auditOf('fb3')).fallback, 'capacity');
  });

  it('a context-length or invalid-request refusal does not fall back', async () => {
    setFallback('acme-llm-secondary', 1);
    script = primaryDoes(status(400, "This model's maximum context length is 131072 tokens."));
    const ctx = await chat('nf1');
    assert.equal(ctx.status, 400);
    assert.equal(ctx.body.error.code, 'CONTEXT_LENGTH');
    script = primaryDoes(status(400, 'tool_choice is not valid'));
    const invalid = await chat('nf2');
    assert.equal(invalid.status, 400);
    assert.equal(invalid.body.error.code, 'INVALID_REQUEST');
    assert.equal(served('acme-llm-secondary').length, 0);
    assert.equal((await auditOf('nf1')).fallback, null);
    assert.equal((await auditOf('nf2')).fallback, null);
  });

  it('a stream cut after its first byte is truncated and does not fall back', async () => {
    setFallback('acme-llm-secondary', 1);
    script = primaryDoes((_p, _b, res) => {
      res.writeHead(200, { 'content-type': 'text/event-stream' });
      frame(res, { choices: [{ delta: { content: 'par' } }] });
      setTimeout(() => res.socket?.destroy(), 100);
    });
    const r = await streamed('cut1');
    assert.match(r.text, /par/);
    assert.doesNotMatch(r.text, /\[DONE\]/);
    assert.equal(served('acme-llm-secondary').length, 0);
    const row = await auditOf('cut1');
    assert.deepEqual([row.outcome, row.fallback], ['truncated', null]);
  });

  it('a request too large for the fallback keeps the original error and is not sent to the fallback', async () => {
    setFallback('acme-llm-secondary', 1);
    script = primaryDoes(silent);
    const big = await chat('big1', { messages: [{ role: 'user', content: longText }] });
    assert.equal(big.status, 504);
    assert.equal(big.body.error.code, 'TIMEOUT');
    assert.equal(served('acme-llm-secondary').length, 0);
    assert.equal((await auditOf('big1')).fallback, null);
  });

  it('a request with tools or images falls back only to a fallback that takes them', async () => {
    setFallback('acme-llm-secondary', 1);
    script = primaryDoes(silent);
    const tools = [{ type: 'function', function: { name: 'f', parameters: {} } }];
    const image = { messages: [{ role: 'user', content: [{ type: 'text', text: 'look' }, { type: 'image_url', image_url: { url: 'data:image/png;base64,AAAA' } }] }] };

    h.cp.db.prepare("UPDATE llm_models SET tools = 0 WHERE model_id = 'acme-llm-secondary'").run();
    assert.equal((await chat('tl1', { tools })).status, 504, 'the fallback has no tools');
    assert.equal(served('acme-llm-secondary').length, 0);

    h.cp.db.prepare("UPDATE llm_models SET tools = 1 WHERE model_id = 'acme-llm-secondary'").run();
    assert.equal((await chat('im1', image)).status, 504, 'the fallback reads no images');
    assert.equal(served('acme-llm-secondary').length, 0);

    h.cp.db.prepare("UPDATE llm_models SET images = 1 WHERE model_id = 'acme-llm-secondary'").run();
    healthy();
    assert.equal((await chat('im2', image)).status, 200, 'the fallback reads images now');
    assert.equal((await auditOf('im2')).fallback, 'timeout');
    assert.equal(served('acme-llm-secondary').length, 1);
  });

  it("the fallback request carries the fallback's stored effort, or none", async () => {
    setFallback('acme-llm-secondary', 1);
    await put('acme-llm-primary', { reasoning_effort: 'low' });
    script = primaryDoes(silent);
    assert.equal((await chat('fx1', { reasoning_effort: 'high' })).status, 200);
    assert.ok(!('reasoning_effort' in served('acme-llm-secondary').at(-1)!.body), "the primary's effort is not sent to the fallback");

    assert.equal((await put('acme-llm-secondary', { reasoning_effort: 'low' })).status, 200);
    healthy();
    a.requests.length = 0;
    assert.equal((await chat('fx2')).status, 200);
    assert.equal(served('acme-llm-secondary').at(-1)!.body.reasoning_effort, 'low');
    await put('acme-llm-primary', { reasoning_effort: null });
    await put('acme-llm-secondary', { reasoning_effort: null });
  });

  it('a primary that answers within the wait is not replaced', async () => {
    setFallback('acme-llm-secondary', 2);
    script = primaryDoes((p, body, res) => setTimeout(() => normal(p, body, res), 300));
    const r = await chat('ok1');
    assert.equal(r.status, 200);
    assert.equal(served('acme-llm-secondary').length, 0);
    const row = await auditOf('ok1');
    assert.deepEqual([row.answered_by, row.fallback], ['acme-llm-primary', null]);
  });

  it('after a fallback, requests go straight to the fallback for the cooldown; then the model is tried again', async () => {
    setFallback('acme-llm-secondary', 1);
    h.cfg.gateway.fallbackCooldownMs = 1500;
    script = primaryDoes(silent);
    assert.equal((await chat('cd1')).status, 200);
    assert.equal(served('acme-llm-primary').length, 1);

    const t0 = Date.now();
    assert.equal((await chat('cd2')).status, 200);
    assert.ok(Date.now() - t0 < 800, 'the cooldown skips the wait');
    assert.equal(served('acme-llm-primary').length, 1, 'the model was not tried');
    const cd2 = await auditOf('cd2');
    assert.deepEqual([cd2.answered_by, cd2.fallback], ['acme-llm-secondary', 'cooldown']);

    // A request the fallback does not fit still tries the model.
    assert.equal((await chat('cd3', { messages: [{ role: 'user', content: longText }] })).status, 504);
    assert.equal(served('acme-llm-primary').length, 2);

    // After the cooldown the model is tried again; a new fallback starts a new cooldown.
    await new Promise((r) => setTimeout(r, 1600));
    assert.equal((await chat('cd4')).status, 200);
    assert.equal(served('acme-llm-primary').length, 3);
    assert.equal((await auditOf('cd4')).fallback, 'timeout');
    assert.equal((await chat('cd5')).status, 200);
    assert.equal(served('acme-llm-primary').length, 3);
    assert.equal((await auditOf('cd5')).fallback, 'cooldown');
  });
});
