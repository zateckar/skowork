// The gateway against the real organization LLM API (OPENAI_BASE_URL / OPENAI_KEY from the repository's .env).
// Local only: never run in CI, and the key is never printed. Run with: npm run test:real
import { after, before, describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { startHarness, type Harness } from '../test/helpers.ts';

const configured = Boolean(process.env.OPENAI_BASE_URL && process.env.OPENAI_KEY);

describe('gateway with the real organization LLM API', { skip: !configured && 'OPENAI_BASE_URL/OPENAI_KEY not set' }, () => {
  let h: Harness;
  let token: string;
  const key = process.env.OPENAI_KEY ?? '';
  const host = configured ? new URL(process.env.OPENAI_BASE_URL!).host : '';

  before(async () => {
    h = await startHarness((c) => {
      c.gateway.upstreams = [{ name: 'organization', baseUrl: process.env.OPENAI_BASE_URL!, apiKey: key, maxConcurrent: 0 }];
    });
    token = await h.token('real1');
  });
  after(() => h?.close());

  const noLeak = (text: string) => {
    assert.ok(!text.includes(key), 'response contains the upstream key');
    assert.ok(!text.includes(host), 'response contains the upstream host');
  };

  it('answers a chat request on the main role', async () => {
    const r = await h.call('POST', '/llm/v1/chat/completions', token, {
      model: 'main', messages: [{ role: 'user', content: 'Reply with exactly the word: pong' }], max_tokens: 400,
    });
    assert.equal(r.status, 200, JSON.stringify(r.body));
    assert.match(JSON.stringify(r.body.choices[0].message.content), /pong/i);
    assert.ok(r.body.usage.total_tokens > 0);
    noLeak(JSON.stringify(r.body));
  });

  it('streams with reasoning delivered separately and ends with [DONE]', async () => {
    const r = await h.call('POST', '/llm/v1/chat/completions', token, {
      model: 'main', stream: true, messages: [{ role: 'user', content: 'What is 17 + 25? Answer with the number only.' }], max_tokens: 800,
    });
    assert.equal(r.status, 200);
    const text = String(r.body);
    assert.match(text, /data: \[DONE\]/);
    assert.match(text, /42/);
    noLeak(text);
    const row = h.cp.db.prepare("SELECT outcome, total_tokens FROM llm_usage WHERE endpoint = 'chat' ORDER BY id DESC LIMIT 1").get() as any;
    assert.equal(row.outcome, 'ok');
    assert.ok(row.total_tokens > 0);
  });

  it('returns a tool call on the small role', async () => {
    const r = await h.call('POST', '/llm/v1/chat/completions', token, {
      model: 'small',
      messages: [{ role: 'user', content: 'Read the file C:/notes/todo.txt' }],
      tools: [{ type: 'function', function: { name: 'files__read', description: 'Read a text file', parameters: { type: 'object', properties: { path: { type: 'string' } }, required: ['path'] } } }],
      max_tokens: 400,
    });
    assert.equal(r.status, 200, JSON.stringify(r.body));
    const call = r.body.choices[0].message.tool_calls?.[0];
    assert.equal(call?.function?.name, 'files__read');
    assert.match(call.function.arguments, /todo\.txt/);
  });

  it('embeds 16 chunks into 16 vectors of 4096 dimensions', async () => {
    const r = await h.call('POST', '/llm/v1/embeddings', token, { model: 'embed', input: Array.from({ length: 16 }, (_, i) => `Kapitola ${i}: Acme Auto`) });
    assert.equal(r.status, 200, JSON.stringify(r.body).slice(0, 300));
    assert.equal(r.body.data.length, 16);
    assert.equal(r.body.dimension, 4096);
  });

  it('generates one image on the image role and counts it in the image budget', { timeout: 150_000 }, async () => {
    const r = await h.call('POST', '/llm/v1/images/generations', token, { model: 'image', prompt: 'A small green electric car on a white background, flat illustration', n: 1 });
    assert.equal(r.status, 200, JSON.stringify(r.body).slice(0, 500));
    assert.equal(r.body.data.length, 1);
    const png = Buffer.from(r.body.data[0].b64_json, 'base64');
    assert.deepEqual([...png.subarray(0, 8)], [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
    noLeak(JSON.stringify({ ...r.body, data: [] }));
    const row = h.cp.db.prepare("SELECT bucket, images, total_tokens, outcome FROM llm_usage WHERE user_name = 'real1' AND endpoint = 'images'").get() as any;
    assert.deepEqual({ ...row }, { bucket: 'image', images: 1, total_tokens: 0, outcome: 'ok' });
  });

  it('reranks 30 passages and returns 30 scores', async () => {
    const r = await h.call('POST', '/llm/v1/rerank', token, {
      model: 'rerank', query: 'electric car battery', documents: Array.from({ length: 30 }, (_, i) => (i === 7 ? 'The Nimbus battery has 82 kWh.' : `Unrelated passage ${i} about cooking.`)),
    });
    assert.equal(r.status, 200, JSON.stringify(r.body).slice(0, 300));
    assert.equal(r.body.results.length, 30);
    const best = [...r.body.results].sort((x: any, y: any) => y.relevance_score - x.relevance_score)[0];
    assert.equal(best.index, 7);
  });
});
