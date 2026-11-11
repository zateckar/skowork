// Spike for add-model-fallback-and-effort: does the organization LLM API accept reasoning_effort, and does it change
// the reasoning? Prints status, reasoning tokens, reasoning length and latency only.
//   node dev/scripts/llm-effort-spike.mjs [model]
import { readFileSync } from 'node:fs';

const env = Object.fromEntries(
  readFileSync(new URL('../../.env', import.meta.url), 'utf8')
    .split(/\r?\n/)
    .map((l) => l.match(/^\s*([A-Za-z_]\w*)\s*=\s*(.*)$/))
    .filter(Boolean)
    .map((m) => [m[1], m[2].trim().replace(/^["']|["']$/g, '')]),
);
const base = env.OPENAI_BASE_URL.replace(/\/+$/, '');
const model = process.argv[2] ?? env.OPENAI_MODEL;
const question = 'A train leaves at 09:40 and travels 214 km at 82 km/h, then stops 7 minutes, then 96 km at 64 km/h. When does it arrive? Answer with the time only.';

for (const effort of [undefined, 'none', 'minimal', 'low', 'medium', 'high', 'bogus']) {
  const body = { model, messages: [{ role: 'user', content: question }], max_tokens: 4000, temperature: 0 };
  if (effort) body.reasoning_effort = effort;
  const t0 = performance.now();
  const r = await fetch(`${base}/chat/completions`, { method: 'POST', headers: { 'content-type': 'application/json', authorization: `Bearer ${env.OPENAI_KEY}` }, body: JSON.stringify(body) });
  const ms = Math.round(performance.now() - t0);
  const j = await r.json().catch(() => ({}));
  const msg = j.choices?.[0]?.message ?? {};
  const reasoning = msg.reasoning_content ?? msg.reasoning ?? '';
  console.log(
    String(effort ?? '(unset)').padEnd(8),
    `status ${r.status}`.padEnd(11),
    `${ms} ms`.padEnd(9),
    `reasoning_tokens ${j.usage?.completion_tokens_details?.reasoning_tokens ?? '-'}`.padEnd(22),
    `reasoning_chars ${reasoning.length}`.padEnd(20),
    r.ok ? `answer ${(msg.content ?? '').trim().slice(0, 20)}` : `error ${JSON.stringify(j.error ?? j).slice(0, 140)}`,
  );
}
