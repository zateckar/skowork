// Spikes 1.1 and 1.2 of fix-long-thread-model-calls, against the organization LLM API from .env.
// Prints timings and token counts only; prompts are generated, and the key is never printed.
//   node dev/scripts/llm-cache-spike.mjs [model] [approxPromptTokens]
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
const model = process.argv[2] ?? env.OPENAI_MODEL;
const target = Number(process.argv[3] ?? 40000);

// Varied filler text so tokens are roughly 1 per 4 characters; a random tag makes every spike run a cold prefix.
const words = ['policy', 'vehicle', 'gearbox', 'supplier', 'invoice', 'meeting', 'battery', 'quality', 'release', 'budget', 'plant', 'audit'];
function filler(tokens, seed) {
  const out = [`Reference document ${seed}.`];
  let n = 0;
  let i = 0;
  while (n < tokens) {
    const line = `Line ${i}: the ${words[i % 12]} report ${i * 7919 % 1000} lists ${words[(i * 5) % 12]} item ${i % 97}.`;
    out.push(line);
    n += Math.ceil(line.length / 4);
    i++;
  }
  return out.join('\n');
}

async function chat(messages, { signal, maxTokens = 16 } = {}) {
  const t0 = performance.now();
  const res = await fetch(`${base}/chat/completions`, {
    method: 'POST',
    headers: { 'content-type': 'application/json', authorization: `Bearer ${env.OPENAI_KEY}` },
    body: JSON.stringify({ model, messages, stream: true, max_tokens: maxTokens, temperature: 0, stream_options: { include_usage: true } }),
    signal,
  });
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  const reader = res.body.getReader();
  const dec = new TextDecoder();
  let ttft = null;
  let usage = null;
  let buf = '';
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    buf += dec.decode(value, { stream: true });
    let i;
    while ((i = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, i).trim();
      buf = buf.slice(i + 1);
      if (!line.startsWith('data:')) continue;
      const data = line.slice(5).trim();
      if (data === '[DONE]') continue;
      const j = JSON.parse(data);
      const d = j.choices?.[0]?.delta;
      if (ttft === null && d && (d.content || d.reasoning_content)) ttft = performance.now() - t0;
      if (j.usage) usage = j.usage;
    }
  }
  return { ttft_ms: Math.round(ttft ?? -1), total_ms: Math.round(performance.now() - t0), prompt: usage?.prompt_tokens, cached: usage?.prompt_tokens_details?.cached_tokens ?? null };
}

const tag = randomBytes(6).toString('hex');
const system = { role: 'system', content: 'You are a terse assistant. Answer with one word.' };
const doc = { role: 'user', content: filler(target, tag) + '\n\nReply with OK.' };
console.log(`model=${model} target≈${target} tokens`);
console.log('1.1 cold      ', await chat([system, doc]));
console.log('1.1 same      ', await chat([system, doc]));
const extended = [system, doc, { role: 'assistant', content: 'OK' }, { role: 'user', content: 'Reply with YES.' }];
console.log('1.1 +1 turn   ', await chat(extended));

// 1.2: does an abandoned long request keep the backend busy?
console.log('1.2 short base', await chat([system, { role: 'user', content: `Say hi ${tag}` }]));
const abort = new AbortController();
const big = chat([system, { role: 'user', content: filler(Math.round(target * 2.5), tag + 'b') + '\nSummarize in detail.' }], { signal: abort.signal, maxTokens: 512 }).catch((e) => e.name);
await new Promise((r) => setTimeout(r, 5000));
abort.abort();
await big;
console.log('1.2 after abort', await chat([system, { role: 'user', content: `Say hello ${tag}` }]));
