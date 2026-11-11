// Acceptance of add-model-fallback-and-effort (task 4.1) against the real organization LLM API: an in-process control
// plane on a temporary in-memory database, a locally signed test admin (no identity provider sign-in), and the LLM
// connection from the repository's .env. Prints only statuses, codes, token counts and audit fields, never the key.
// Usage (from control-plane/): node scripts/accept-fallback.ts
import { randomBytes } from 'node:crypto';
import { startHarness } from '../test/helpers.ts';

process.loadEnvFile(new URL('../../.env', import.meta.url));
const PRIMARY = process.env.OPENAI_MODEL || 'skoda-llm-primary';
const SECONDARY = PRIMARY.replace('primary', 'secondary');
const QUESTION = 'A train leaves at 9:47 and the trip takes 4 hours and 7 minutes. When does it arrive? Answer with the time only.';

const h = await startHarness((c) => {
  c.gateway.upstreams = [{ name: 'primary', baseUrl: process.env.OPENAI_BASE_URL!, apiKey: process.env.OPENAI_KEY ?? '', maxConcurrent: 0 }];
  c.gateway.models = { ...c.gateway.models, main: PRIMARY, small: SECONDARY };
  c.gateway.capabilities[PRIMARY] = { ...c.gateway.capabilities['acme-llm-primary']! };
  c.gateway.capabilities[SECONDARY] = { ...c.gateway.capabilities['acme-llm-secondary']!, reasoning: true };
});
const admin = await h.token('admin1', {}, { lifetimeS: 590 });
const ad = (method: string, path: string, body?: unknown) => h.call(method, `/api/v1/admin/llm${path}`, admin, body);
const log = (label: string, v: unknown) => console.log(`${label}: ${JSON.stringify(v)}`);

try {
  const list = await ad('GET', '/connections');
  if (!list.body?.connections?.length) throw new Error(`connections: ${list.status} ${JSON.stringify(list.body?.error ?? list.body).slice(0, 300)}`);
  const conn = list.body.connections[0];
  const test = await ad('POST', `/connections/${conn.id}/test`);
  log('connection test', { status: test.status, ok: test.body.ok, effort_failures: test.body.effort_failures });
  const put = (model: string, body: object) => ad('PUT', `/connections/${conn.id}/models/${model}`, body);

  const ask = async () => {
    const t0 = Date.now();
    const r = await h.call('POST', '/llm/v1/chat/completions', admin, { model: 'main', max_tokens: 4000, temperature: 0, messages: [{ role: 'user', content: QUESTION }] });
    return {
      status: r.status,
      seconds: (Date.now() - t0) / 1000,
      reasoning_tokens: r.body?.usage?.completion_tokens_details?.reasoning_tokens ?? null,
      answer: String(r.body?.choices?.[0]?.message?.content ?? r.body?.error?.code ?? '').trim().slice(0, 40),
    };
  };

  // 1. Effort low on the primary.
  log('primary, effort unset', await ask());
  const low = await put(PRIMARY, { reasoning_effort: 'low' });
  log('set primary effort low', { status: low.status, error: low.body?.error });
  log('primary, effort low', await ask());

  // 2. Effort high on the secondary is refused.
  const high = await put(SECONDARY, { reasoning_effort: 'high' });
  log('set secondary effort high', { status: high.status, code: high.body?.error?.code, field: high.body?.error?.field, reason: high.body?.error?.reason, message: high.body?.error?.message });

  // 3. The secondary as fallback of the primary with a 5 s wait; a long request, then a short one during the cooldown.
  await put(PRIMARY, { reasoning_effort: null });
  const fb = await put(PRIMARY, { fallback_model: SECONDARY, fallback_after_s: 5 });
  log('set fallback', { status: fb.status, error: fb.body?.error });
  const row = (await ad('GET', '/connections')).body.connections[0].models.filter((m: any) => [PRIMARY, SECONDARY].includes(m.id));
  log('model list', row.map((m: any) => ({ id: m.id, reasoning_effort: m.reasoning_effort, fallback_model: m.fallback_model, fallback_after_s: m.fallback_after_s, fallback_of: m.fallback_of })));

  // Random words defeat the prefix cache, so the primary has to prefill all of it (about 45,000 estimated tokens).
  const words = Array.from({ length: 30_000 }, () => randomBytes(3).toString('hex')).join(' ');
  const streamChat = async (content: string) => {
    const t0 = Date.now();
    const r = await fetch(h.base + '/llm/v1/chat/completions', {
      method: 'POST',
      headers: { authorization: `Bearer ${admin}`, 'content-type': 'application/json' },
      body: JSON.stringify({ model: 'main', stream: true, max_tokens: 200, messages: [{ role: 'user', content }] }),
    });
    const text = await r.text();
    const errors = text.split('\n\n').filter((f) => f.startsWith('event: error') || f.includes('"error"')).length;
    return { status: r.status, seconds: (Date.now() - t0) / 1000, keep_alives: text.split(': keep-alive').length - 1, error_events: errors, done: text.includes('data: [DONE]') };
  };
  log('long request', await streamChat(`${words}\n\nHow many words are above? Answer with a number only.`));
  log('short request during cooldown', await streamChat(QUESTION));

  await new Promise((r) => setTimeout(r, 500));
  const rows = h.cp.db.prepare("SELECT upstream_model, answered_by, fallback, outcome, ttft_ms, prompt_tokens, reasoning_tokens FROM llm_usage WHERE endpoint = 'chat' ORDER BY id").all();
  log('audit', rows);
} finally {
  await h.close();
}
