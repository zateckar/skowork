// Spike 1.3 / acceptance 4.1 of fix-long-thread-model-calls: time to first token per Turn for a growing Thread,
// sent straight to the organization LLM API from .env (no client, no sign-in).
//   node dev/scripts/llm-thread-spike.mjs [old|new] [turns] [tokensPerTurn] [model]
// "old": the time line sits in the system prompt and changes every Turn (before the change).
// "new": the system prompt is stable and each user message carries its own time line (design D4).
import { readFileSync } from 'node:fs';
import { randomBytes } from 'node:crypto';

const env = Object.fromEntries(
  readFileSync(new URL('../../.env', import.meta.url), 'utf8')
    .split(/\r?\n/)
    .map((l) => l.match(/^\s*([A-Za-z_]\w*)\s*=\s*(.*)$/))
    .filter(Boolean)
    .map((m) => [m[1], m[2].trim().replace(/^["']|["']$/g, '')]),
);
const base = env.OPENAI_BASE_URL.replace(/\/+$/, '');
const mode = process.argv[2] ?? 'new';
const turns = Number(process.argv[3] ?? 20);
const perTurn = Number(process.argv[4] ?? 5000);
const model = process.argv[5] ?? env.OPENAI_MODEL;
const tag = randomBytes(6).toString('hex');

function toolResult(turn) {
  const lines = [`Search results ${tag}/${turn}:`];
  for (let i = 0, n = 0; n < perTurn; i++) {
    const l = `- result ${turn}.${i}: document ${(turn * 131 + i * 7919) % 10007} mentions item ${i % 89} in section ${i % 13}.`;
    lines.push(l);
    n += Math.ceil(l.length / 4);
  }
  return lines.join('\n');
}

async function chat(messages) {
  const t0 = performance.now();
  const res = await fetch(`${base}/chat/completions`, {
    method: 'POST',
    headers: { 'content-type': 'application/json', authorization: `Bearer ${env.OPENAI_KEY}` },
    body: JSON.stringify({ model, messages, stream: true, max_tokens: 8, temperature: 0, stream_options: { include_usage: true } }),
  });
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  const reader = res.body.getReader();
  const dec = new TextDecoder();
  let ttft = null, usage = null, buf = '';
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    if (ttft === null) ttft = performance.now() - t0;
    buf += dec.decode(value, { stream: true });
    for (let i; (i = buf.indexOf('\n')) >= 0; ) {
      const line = buf.slice(0, i).trim();
      buf = buf.slice(i + 1);
      if (line.startsWith('data:') && !line.includes('[DONE]')) {
        const j = JSON.parse(line.slice(5));
        if (j.usage) usage = j.usage;
      }
    }
  }
  return { ttft: Math.round(ttft), prompt: usage?.prompt_tokens ?? 0, cached: usage?.prompt_tokens_details?.cached_tokens ?? 0 };
}

const basePrompt = 'You are Skowork, a helpful assistant. Answer in one short word.';
const stable = '- User: test.user\n- Time zone: W. Europe Standard Time (UTC+01:00)\n- Documents: C:/Users/test/Documents';
const history = [];
console.log(`mode=${mode} model=${model} turns=${turns} ≈${perTurn} tokens per Turn`);
for (let t = 1; t <= turns; t++) {
  const now = `Tuesday 10 November 2026, 10:${String(t).padStart(2, '0')} (UTC+01:00)`;
  const system =
    mode === 'old'
      ? `${basePrompt}\n\nContext of this Run:\n- Now: ${now}\n${stable}\n\nUntrusted data instructions.`
      : `${basePrompt}\n\nContext of this Run:\n${stable}\n\nUntrusted data instructions.`;
  const userText = mode === 'old' ? `Question ${t}: summarize the results.` : `Current time: ${now}\nQuestion ${t}: summarize the results.`;
  history.push({ role: 'user', content: userText });
  history.push({ role: 'assistant', content: 'Searching.' });
  history.push({ role: 'user', content: toolResult(t) });
  const r = await chat([{ role: 'system', content: system }, ...history]);
  history.push({ role: 'assistant', content: 'Done.' });
  console.log(`turn ${String(t).padStart(2)}  prompt ${String(r.prompt).padStart(6)}  cached ${String(r.cached).padStart(6)}  ttft ${r.ttft} ms`);
}
